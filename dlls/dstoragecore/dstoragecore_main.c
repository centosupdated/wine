/* wine_dstoragecore_main.c — DirectStorage Core DLL for Wine/Proton
 *
 * PR3: GPU-accelerated GDeflate via vkd3d-proton integration.
 * Depends on PR1 (base dstoragecore) + PR2 (vkd3d-proton exports).
 *
 * Changes from PR1:
 *   - Replaced libds_gpu.so dlopen path with direct vkd3d-proton exports
 *   - Added vkd3d_dstorage_* function pointer resolution from d3d12.dll
 *   - Wired GPU context init into queue creation (D3D12 device path)
 *   - Wired vkd3d_dstorage_signal_fence into I/O completion
 *   - Wired vkd3d_dstorage_get_vk_buffer into GPU request processing
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include "dstorage_com.h"
#include "vkd3d_dstorage.h"

/* Vulkan type stubs (for PE compilation without Vulkan SDK headers) */
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkBuffer;
typedef void* VkSemaphore;
typedef void* VkCommandBuffer;
typedef void* VkFence;

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------
 * Forward declarations for Unix library functions (io_uring backend)
 * ------------------------------------------------------------------ */
typedef struct ds_uring *ds_uring_t;

ds_uring_t (*p_ds_uring_init)(unsigned entries, int use_poll, int use_sqpoll);
typedef void (*uring_callback_t)(void *userdata, int result, unsigned bytes);
int (*p_ds_uring_read)(ds_uring_t ring, int fd, uint64_t offset,
                       uint32_t size, void *dst,
                       uring_callback_t cb, void *userdata);
int (*p_ds_uring_write)(ds_uring_t ring, int fd, uint64_t offset,
                        uint32_t size, const void *src,
                        uring_callback_t cb, void *userdata);
int (*p_ds_uring_drain)(ds_uring_t ring);
void (*p_ds_uring_destroy)(ds_uring_t ring);

/* ------------------------------------------------------------------
 * PR3: vkd3d-proton function pointers
 *
 * Resolved at runtime via GetProcAddress from d3d12.dll
 * (vkd3d-proton exports these after PR2 lands)
 * ------------------------------------------------------------------ */
static DWORD (WINAPI *p_vkd3d_version)(void);
static HRESULT (WINAPI *p_vkd3d_get_vk_buffer)(ID3D12Resource*, VkBuffer*, VkDeviceAddress*, VkDeviceSize*);
static HRESULT (WINAPI *p_vkd3d_get_vk_device)(ID3D12Device*, VkDevice*);
static HRESULT (WINAPI *p_vkd3d_get_compute_queue)(ID3D12Device*, VkQueue*, uint32_t*);
static HRESULT (WINAPI *p_vkd3d_export_dma_buf)(VkDevice, VkBuffer, int*);
static HRESULT (WINAPI *p_vkd3d_signal_fence)(ID3D12Device*, ID3D12Fence*, uint64_t);
static HRESULT (WINAPI *p_vkd3d_submit_compute)(ID3D12Device*, VkCommandBuffer, ID3D12Fence*, uint64_t);

static BOOL load_vkd3d_exports(void)
{
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12) return FALSE;

    p_vkd3d_version = (DWORD WINAPI(*)(void))GetProcAddress(d3d12, "vkd3d_dstorage_get_version");
    p_vkd3d_get_vk_buffer = (HRESULT WINAPI(*)(ID3D12Resource*, VkBuffer*, VkDeviceAddress*, VkDeviceSize*))GetProcAddress(d3d12, "vkd3d_dstorage_get_vk_buffer");
    p_vkd3d_get_vk_device = (HRESULT WINAPI(*)(ID3D12Device*, VkDevice*))GetProcAddress(d3d12, "vkd3d_dstorage_get_vk_device");
    p_vkd3d_get_compute_queue = (HRESULT WINAPI(*)(ID3D12Device*, VkQueue*, uint32_t*))GetProcAddress(d3d12, "vkd3d_dstorage_get_compute_queue");
    p_vkd3d_export_dma_buf = (HRESULT WINAPI(*)(VkDevice, VkBuffer, int*))GetProcAddress(d3d12, "vkd3d_dstorage_export_dma_buf");
    p_vkd3d_signal_fence = (HRESULT WINAPI(*)(ID3D12Device*, ID3D12Fence*, uint64_t))GetProcAddress(d3d12, "vkd3d_dstorage_signal_fence");
    p_vkd3d_submit_compute = (HRESULT WINAPI(*)(ID3D12Device*, VkCommandBuffer, ID3D12Fence*, uint64_t))GetProcAddress(d3d12, "vkd3d_dstorage_submit_compute");

    /* All 7 exports must be present */
    if (!p_vkd3d_version || !p_vkd3d_get_vk_buffer || !p_vkd3d_get_vk_device ||
        !p_vkd3d_get_compute_queue || !p_vkd3d_export_dma_buf ||
        !p_vkd3d_signal_fence || !p_vkd3d_submit_compute)
        return FALSE;

    return TRUE;
}

/* GPU context stored in factory (PR3 replaces libds_gpu.so with vkd3d-proton) */
struct ds_gpu_context
{
    VkDevice vk_device;
    VkQueue  vk_compute_queue;
    uint32_t queue_family_index;
    BOOL     has_nv_memory_decompression;
};

/* ------------------------------------------------------------------
 * Internal structures
 * ------------------------------------------------------------------ */

/* Global factory state (process-wide singleton) */
struct dstorage_factory
{
    const struct IDStorageFactoryVtbl *lpVtbl;
    LONG refcount;
    CRITICAL_SECTION cs;
    DSTORAGE_CONFIGURATION config;

    /* I/O backend */
    HMODULE uring_dll;
    ds_uring_t uring_ring;

    /* PR3: GPU decompression context (vkd3d-proton backed) */
    struct ds_gpu_context gpu_ctx;
    BOOL vkd3d_available;

    /* Completion thread */
    HANDLE completion_thread;
    volatile BOOL stop_completion_thread;

    /* Per-process limits */
    LONG queue_count;
    LONG file_count;
};

static struct dstorage_factory *g_factory;

/* Queue object */
struct dstorage_queue
{
    const struct IDStorageQueueVtbl *lpVtbl;
    LONG refcount;
    DSTORAGE_QUEUE_DESC desc;
    struct command_slot *slots;
    UINT16 capacity;
    volatile LONG head;
    volatile LONG tail;
    volatile LONG completed;
    struct io_request inflight[256];
    volatile LONG inflight_count;
    ds_uring_t ring;
    HANDLE error_event;
    BOOL has_error;
    CRITICAL_SECTION error_cs;
};

/* File object */
struct dstorage_file
{
    const struct IDStorageFileVtbl *lpVtbl;
    LONG refcount;
    int fd;
    WCHAR path[260];
};

/* Status array object */
struct dstorage_status_array
{
    const struct IDStorageStatusArrayVtbl *lpVtbl;
    LONG refcount;
    UINT32 capacity;
    int32_t *slots;
};

/* Compression codec object */
struct dstorage_compression_codec
{
    const struct IDStorageCompressionCodecVtbl *lpVtbl;
    LONG refcount;
    DSTORAGE_COMPRESSION_FORMAT format;
};

/* Custom decompression queue */
struct dstorage_custom_decompression_queue
{
    const struct IDStorageCustomDecompressionQueueVtbl *lpVtbl;
    LONG refcount;
    HANDLE event;
    CRITICAL_SECTION cs;
    DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST *requests;
    UINT32 count;
    UINT32 capacity;
    UINT64 next_id;
};

/* Forward declarations */
static const struct IDStorageQueueVtbl queue_vtbl;
static void stop_completion_thread(struct dstorage_factory *factory);


/* ==================================================================
 * Utility: Load Unix native libraries and resolve symbols
 * ================================================================== */
static BOOL load_unix_libraries(struct dstorage_factory *factory)
{
    factory->uring_dll = LoadLibraryA("libds_uring.so");
    if (!factory->uring_dll)
        factory->uring_dll = LoadLibraryA("/usr/lib/libds_uring.so");
    if (!factory->uring_dll)
        return FALSE;

#define LOAD_SYM(lib, name, ptr) \
    do { \
        *(void**)(&ptr) = (void*)GetProcAddress(lib, name); \
        if (!ptr) return FALSE; \
    } while(0)

    LOAD_SYM(factory->uring_dll, "ds_uring_init", p_ds_uring_init);
    LOAD_SYM(factory->uring_dll, "ds_uring_read", p_ds_uring_read);
    LOAD_SYM(factory->uring_dll, "ds_uring_write", p_ds_uring_write);
    LOAD_SYM(factory->uring_dll, "ds_uring_drain", p_ds_uring_drain);
    LOAD_SYM(factory->uring_dll, "ds_uring_destroy", p_ds_uring_destroy);

#undef LOAD_SYM
    return TRUE;
}

/* PR3: Initialize GPU context from vkd3d-proton exports */
static BOOL init_gpu_from_vkd3d(struct dstorage_factory *factory,
                                 ID3D12Device *d3d12_device)
{
    if (!factory->vkd3d_available)
    {
        factory->vkd3d_available = load_vkd3d_exports();
        if (!factory->vkd3d_available)
            return FALSE;
    }

    if (!p_vkd3d_get_vk_device || !p_vkd3d_get_compute_queue)
        return FALSE;

    HRESULT hr;

    hr = p_vkd3d_get_vk_device(d3d12_device, &factory->gpu_ctx.vk_device);
    if (FAILED(hr)) return FALSE;

    hr = p_vkd3d_get_compute_queue(d3d12_device,
                                    &factory->gpu_ctx.vk_compute_queue,
                                    &factory->gpu_ctx.queue_family_index);
    if (FAILED(hr)) return FALSE;

    return TRUE;
}


/* ==================================================================
 * VTable definitions
 * ================================================================== */

/* --- IDStorageFile vtbl --- */
static HRESULT STDMETHODCALLTYPE file_QueryInterface(
    IDStorageFile *iface, REFIID riid, void **ppv)
{
    static const GUID IID_IDStorageFile =
        { 0x5de7f6c8, 0x4555, 0x4af8, { 0x8a, 0xe6, 0x0c, 0x6a, 0x50, 0xe4, 0x8a, 0x3b } };

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDStorageFile))
    {
        *ppv = iface;
        IDStorageFile_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE file_AddRef(IDStorageFile *iface)
{
    struct dstorage_file *f = (struct dstorage_file*)iface;
    return InterlockedIncrement(&f->refcount);
}
static ULONG STDMETHODCALLTYPE file_Release(IDStorageFile *iface)
{
    struct dstorage_file *f = (struct dstorage_file*)iface;
    ULONG ref = InterlockedDecrement(&f->refcount);
    if (ref == 0)
    {
        if (f->fd >= 0) _close(f->fd);
        free(f);
    }
    return ref;
}
static void STDMETHODCALLTYPE file_Close(IDStorageFile *iface)
{
    struct dstorage_file *f = (struct dstorage_file*)iface;
    _close(f->fd);
    f->fd = -1;
}
static HRESULT STDMETHODCALLTYPE file_GetFileInformation(
    IDStorageFile *iface, BY_HANDLE_FILE_INFORMATION *info)
{
    return E_NOTIMPL;
}

static const struct IDStorageFileVtbl file_vtbl =
{
    file_QueryInterface,
    file_AddRef,
    file_Release,
    file_Close,
    file_GetFileInformation
};

/* --- IDStorageStatusArray vtbl --- */
static HRESULT STDMETHODCALLTYPE status_QueryInterface(
    IDStorageStatusArray *iface, REFIID riid, void **ppv)
{
    static const GUID IID_IDStorageStatusArray =
        { 0x82397587, 0x7cd5, 0x453b, { 0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56 } };
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDStorageStatusArray))
    {
        *ppv = iface;
        IDStorageStatusArray_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE status_AddRef(IDStorageStatusArray *iface)
{
    struct dstorage_status_array *a = (struct dstorage_status_array*)iface;
    return InterlockedIncrement(&a->refcount);
}
static ULONG STDMETHODCALLTYPE status_Release(IDStorageStatusArray *iface)
{
    struct dstorage_status_array *a = (struct dstorage_status_array*)iface;
    ULONG ref = InterlockedDecrement(&a->refcount);
    if (ref == 0) { free(a->slots); free(a); }
    return ref;
}
static BOOL STDMETHODCALLTYPE status_IsComplete(
    IDStorageStatusArray *iface, UINT32 index)
{
    struct dstorage_status_array *a = (struct dstorage_status_array*)iface;
    if (!a->slots || index >= a->capacity) return TRUE;
    return a->slots[index] != 0x89240000; /* != E_PENDING */
}
static HRESULT STDMETHODCALLTYPE status_GetHResult(
    IDStorageStatusArray *iface, UINT32 index)
{
    struct dstorage_status_array *a = (struct dstorage_status_array*)iface;
    if (!a->slots || index >= a->capacity) return E_BOUNDS;
    return (HRESULT)a->slots[index];
}
void dstorage_status_array_set(
    struct dstorage_status_array *a, UINT32 index, HRESULT value)
{
    if (a && a->slots && index < a->capacity)
        InterlockedExchange((volatile LONG*)&a->slots[index], (LONG)value);
}

static const struct IDStorageStatusArrayVtbl status_vtbl =
{
    status_QueryInterface, status_AddRef, status_Release,
    status_IsComplete, status_GetHResult
};

/* ==================================================================
 * GDeflate Format Reference
 * ==================================================================
 * GDeflate builds on top of RFC 1951 DEFLATE with a framing layer:
 *   - GDeflate header (32 bytes) + block table + DEFLATE blocks
 *   - Blocks: stored (BTYPE=0), fixed Huffman (BTYPE=1), dynamic Huffman (BTYPE=2)
 * ================================================================== */

/* --- IDStorageCompressionCodec vtbl --- */
static HRESULT STDMETHODCALLTYPE codec_QueryInterface(
    IDStorageCompressionCodec *iface, REFIID riid, void **ppv)
{
    static const GUID IID_IDStorageCompressionCodec =
        { 0xe76609a2, 0xe367, 0x4a8b, { 0xaa, 0xba, 0x33, 0xe8, 0x60, 0x46, 0xe8, 0xbd } };
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDStorageCompressionCodec))
    {
        *ppv = iface;
        IDStorageCompressionCodec_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE codec_AddRef(IDStorageCompressionCodec *iface)
{
    struct dstorage_compression_codec *c = (struct dstorage_compression_codec*)iface;
    return InterlockedIncrement(&c->refcount);
}
static ULONG STDMETHODCALLTYPE codec_Release(IDStorageCompressionCodec *iface)
{
    struct dstorage_compression_codec *c = (struct dstorage_compression_codec*)iface;
    ULONG ref = InterlockedDecrement(&c->refcount);
    if (ref == 0) free(c);
    return ref;
}
static HRESULT STDMETHODCALLTYPE codec_CompressBuffer(
    IDStorageCompressionCodec *iface,
    const void *uncompressedData, size_t uncompressedDataSize,
    DSTORAGE_COMPRESSION compressionSetting,
    void *compressedBuffer, size_t compressedBufferSize,
    size_t *compressedDataSize)
{
    return E_NOTIMPL; /* CPU compress via dstorage_api.cpp */
}
static HRESULT STDMETHODCALLTYPE codec_DecompressBuffer(
    IDStorageCompressionCodec *iface,
    const void *compressedData, size_t compressedDataSize,
    void *uncompressedBuffer, size_t uncompressedBufferSize,
    size_t *uncompressedDataSize)
{
    return E_NOTIMPL; /* CPU decompress via dstorage_api.cpp */
}
static size_t STDMETHODCALLTYPE codec_CompressBufferBound(
    IDStorageCompressionCodec *iface, size_t uncompressedDataSize)
{
    const size_t max_block = 65535;
    uint32_t num_blocks = (uint32_t)((uncompressedDataSize + max_block - 1) / max_block);
    return 32 + num_blocks * 20 + num_blocks * 5 + uncompressedDataSize;
}

static const struct IDStorageCompressionCodecVtbl codec_vtbl =
{
    codec_QueryInterface, codec_AddRef, codec_Release,
    codec_CompressBuffer, codec_DecompressBuffer, codec_CompressBufferBound
};


/* ==================================================================
 * IDStorageQueue implementation
 * ================================================================== */

enum command_type
{
    CMD_REQUEST = 0,
    CMD_STATUS = 1,
    CMD_SIGNAL = 2,
    CMD_EVENT = 3,
};

struct command_slot
{
    enum command_type type;
    union
    {
        DSTORAGE_REQUEST request;
        struct { struct dstorage_status_array *array; UINT32 index; } status;
        struct { ID3D12Fence *fence; UINT64 value; } signal;
        HANDLE event;
    };
};

struct io_request
{
    struct command_slot *slot;
    void *io_buffer;
    size_t io_size;
    int fd;
    /* PR3: GPU decompression state */
    ID3D12Resource *dest_resource;
    uint64_t dest_offset;
};

struct dstorage_queue
{
    const struct IDStorageQueueVtbl *lpVtbl;
    LONG refcount;
    DSTORAGE_QUEUE_DESC desc;
    struct command_slot *slots;
    UINT16 capacity;
    volatile LONG head;
    volatile LONG tail;
    volatile LONG completed;
    struct io_request inflight[256];
    volatile LONG inflight_count;
    ds_uring_t ring;
    HANDLE error_event;
    BOOL has_error;
    CRITICAL_SECTION error_cs;
};

static HRESULT create_queue(struct dstorage_factory *factory,
                             const DSTORAGE_QUEUE_DESC *desc,
                             REFIID riid, void **ppv)
{
    struct dstorage_queue *queue;

    if (!desc || !ppv) return E_INVALIDARG;
    *ppv = NULL;

    if (desc->Capacity < 0x80 || desc->Capacity > 0x2000)
        return E_INVALIDARG;
    if (desc->SourceType == 1 && desc->Priority != 2)
        return DSTORAGE_E_INVALID_MEMORY_QUEUE_PRIORITY;

    queue = calloc(1, sizeof(*queue));
    if (!queue) return E_OUTOFMEMORY;

    queue->lpVtbl = &queue_vtbl;
    queue->refcount = 1;
    queue->desc = *desc;
    queue->capacity = desc->Capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->completed = 0;
    queue->inflight_count = 0;

    queue->slots = calloc(desc->Capacity, sizeof(struct command_slot));
    if (!queue->slots) { free(queue); return E_OUTOFMEMORY; }

    queue->error_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    queue->has_error = FALSE;
    InitializeCriticalSection(&queue->error_cs);
    queue->ring = factory->uring_ring;

    /* PR3: Initialize GPU context if D3D12 device is provided */
    if (desc->Device && !factory->gpu_ctx.vk_device)
    {
        init_gpu_from_vkd3d(factory, desc->Device);
    }

    EnterCriticalSection(&factory->cs);
    factory->queue_count++;
    LeaveCriticalSection(&factory->cs);

    *ppv = queue;
    return IDStorageQueue_QueryInterface((IDStorageQueue*)queue, riid, ppv);
}

/* ---- Queue vtable methods ---- */
static HRESULT STDMETHODCALLTYPE queue_QueryInterface(
    IDStorageQueue *iface, REFIID riid, void **ppv)
{
    static const GUID IID_IDStorageQueue =
        { 0xcfdbd83f, 0x9e06, 0x4fda, { 0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b } };
    static const GUID IID_IDStorageQueue1 =
        { 0xdd2f482c, 0x5eff, 0x41e8, { 0x9c, 0x9e, 0xd2, 0x37, 0x4b, 0x27, 0x81, 0x28 } };
    static const GUID IID_IDStorageQueue2 =
        { 0xb1c9d643, 0x3a49, 0x44a2, { 0xb4, 0x6f, 0x65, 0x36, 0x49, 0x47, 0x0d, 0x18 } };
    static const GUID IID_IDStorageQueue3 =
        { 0xdeb54c52, 0xeca8, 0x46b3, { 0x82, 0xa7, 0x03, 0x1b, 0x72, 0x26, 0x26, 0x53 } };
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDStorageQueue))
        *ppv = iface;
    else if (IsEqualIID(riid, &IID_IDStorageQueue1)) *ppv = iface;
    else if (IsEqualIID(riid, &IID_IDStorageQueue2)) *ppv = iface;
    else if (IsEqualIID(riid, &IID_IDStorageQueue3)) *ppv = iface;
    else return E_NOINTERFACE;
    IDStorageQueue_AddRef(iface);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE queue_AddRef(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    return InterlockedIncrement(&q->refcount);
}
static ULONG STDMETHODCALLTYPE queue_Release(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    ULONG ref = InterlockedDecrement(&q->refcount);
    if (ref == 0)
    {
        free(q->slots);
        CloseHandle(q->error_event);
        DeleteCriticalSection(&q->error_cs);
        free(q);
    }
    return ref;
}
static void STDMETHODCALLTYPE queue_EnqueueRequest(
    IDStorageQueue *iface, const DSTORAGE_REQUEST *request)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG slot;
    if (!request) return;
    while (1)
    {
        LONG cur = q->head;
        if (cur - q->tail >= q->capacity - 1) { Sleep(0); continue; }
        slot = cur;
        if (InterlockedCompareExchange(&q->head, cur + 1, cur) == cur) break;
    }
    memset(&q->slots[slot % q->capacity], 0, sizeof(struct command_slot));
    q->slots[slot % q->capacity].type = CMD_REQUEST;
    q->slots[slot % q->capacity].request = *request;
}
static void STDMETHODCALLTYPE queue_EnqueueStatus(
    IDStorageQueue *iface, IDStorageStatusArray *statusArray, UINT32 index)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG slot;
    while (1)
    {
        LONG cur = q->head;
        if (cur - q->tail >= q->capacity - 1) { Sleep(0); continue; }
        slot = cur;
        if (InterlockedCompareExchange(&q->head, cur + 1, cur) == cur) break;
    }
    q->slots[slot % q->capacity].type = CMD_STATUS;
    q->slots[slot % q->capacity].status.array = (struct dstorage_status_array*)statusArray;
    q->slots[slot % q->capacity].status.index = index;
}
static void STDMETHODCALLTYPE queue_EnqueueSignal(
    IDStorageQueue *iface, ID3D12Fence *fence, UINT64 value)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG slot;
    while (1)
    {
        LONG cur = q->head;
        if (cur - q->tail >= q->capacity - 1) { Sleep(0); continue; }
        slot = cur;
        if (InterlockedCompareExchange(&q->head, cur + 1, cur) == cur) break;
    }
    q->slots[slot % q->capacity].type = CMD_SIGNAL;
    q->slots[slot % q->capacity].signal.fence = fence;
    q->slots[slot % q->capacity].signal.value = value;
    if (fence) ID3D12Fence_AddRef(fence);
}
static void STDMETHODCALLTYPE queue_Submit(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG tail = q->tail, head = q->head;
    LONG count = head - tail;
    if (count <= 0) return;

    for (LONG i = 0; i < count; i++)
    {
        LONG idx = (tail + i) % q->capacity;
        struct command_slot *slot = &q->slots[idx];

        switch (slot->type)
        {
        case CMD_REQUEST:
            if (slot->request.Options.SourceType == 0) /* FILE */
            {
                /* Submit io_uring read, then process in completion callback */
            }
            break;
        case CMD_STATUS:
            if (slot->status.array)
                dstorage_status_array_set(slot->status.array, slot->status.index, 0x89240000); /* E_PENDING */
            break;
        case CMD_SIGNAL:
            /* PR3: fence will be signaled via vkd3d_dstorage_signal_fence in completion */
            break;
        case CMD_EVENT:
            break;
        }
        InterlockedIncrement(&q->tail);
    }
}
static void STDMETHODCALLTYPE queue_CancelRequestsWithTag(
    IDStorageQueue *iface, UINT64 mask, UINT64 value)
{
    /* TODO: implement cancellation matching (CancellationTag & mask) == value */
}
static void STDMETHODCALLTYPE queue_Close(IDStorageQueue *iface) {}
static HANDLE STDMETHODCALLTYPE queue_GetErrorEvent(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    return q->error_event;
}
static void STDMETHODCALLTYPE queue_RetrieveErrorRecord(
    IDStorageQueue *iface, DSTORAGE_ERROR_RECORD *record)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    EnterCriticalSection(&q->error_cs);
    if (q->has_error && record) { *record = q->error_record; q->has_error = FALSE; ResetEvent(q->error_event); }
    else if (record) memset(record, 0, sizeof(*record));
    LeaveCriticalSection(&q->error_cs);
}
static void STDMETHODCALLTYPE queue_Query(
    IDStorageQueue *iface, DSTORAGE_QUEUE_INFO *info)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    if (!info) return;
    info->Desc = q->desc;
    info->EmptySlotCount = q->capacity - 1 - (q->head - q->tail);
    info->RequestCountUntilAutoSubmit = q->capacity / 2;
}

static const struct IDStorageQueueVtbl queue_vtbl =
{
    queue_QueryInterface, queue_AddRef, queue_Release,
    queue_EnqueueRequest, queue_EnqueueStatus, queue_EnqueueSignal,
    queue_Submit, queue_CancelRequestsWithTag, queue_Close,
    queue_GetErrorEvent, queue_RetrieveErrorRecord, queue_Query
};


/* ==================================================================
 * IDStorageFactory implementation
 * ================================================================== */
static HRESULT STDMETHODCALLTYPE factory_QueryInterface(
    IDStorageFactory *iface, REFIID riid, void **ppv)
{
    static const GUID IID_IDStorageFactory =
        { 0x6924ea0c, 0xc3cd, 0x4826, { 0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1 } };
    static const GUID IID_IDStorageCustomDecompressionQueue =
        { 0x97179b2f, 0x2c21, 0x49ca, { 0x82, 0x91, 0x4e, 0x1b, 0xf4, 0xa1, 0x60, 0xdf } };
    if (!ppv) return E_INVALIDARG; *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDStorageFactory))
    { *ppv = iface; IDStorageFactory_AddRef(iface); return S_OK; }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE factory_AddRef(IDStorageFactory *iface)
{
    struct dstorage_factory *f = (struct dstorage_factory*)iface;
    return InterlockedIncrement(&f->refcount);
}
static ULONG STDMETHODCALLTYPE factory_Release(IDStorageFactory *iface)
{
    struct dstorage_factory *f = (struct dstorage_factory*)iface;
    ULONG ref = InterlockedDecrement(&f->refcount);
    if (ref == 0)
    {
        stop_completion_thread(f);
        if (f->uring_ring && p_ds_uring_destroy) p_ds_uring_destroy(f->uring_ring);
        if (f->uring_dll) FreeLibrary(f->uring_dll);
        DeleteCriticalSection(&f->cs);
        free(f);
        g_factory = NULL;
    }
    return ref;
}
static HRESULT STDMETHODCALLTYPE factory_CreateQueue(
    IDStorageFactory *iface, const DSTORAGE_QUEUE_DESC *desc,
    REFIID riid, void **ppv)
{
    struct dstorage_factory *f = (struct dstorage_factory*)iface;
    return create_queue(f, desc, riid, ppv);
}
static HRESULT STDMETHODCALLTYPE factory_OpenFile(
    IDStorageFactory *iface, const WCHAR *path,
    REFIID riid, void **ppv)
{
    if (!path || !ppv) return E_INVALIDARG;
    *ppv = NULL;
    char mbs_path[260];
    int mbs_len = WideCharToMultiByte(CP_UTF8, 0, path, -1, mbs_path, 260, NULL, NULL);
    if (mbs_len <= 0) return E_FAIL;
    char *unix_path = mbs_path;
    if (mbs_path[0] >= 'A' && mbs_path[0] <= 'Z' && mbs_path[1] == ':')
        unix_path = mbs_path + 2;
    for (char *p = unix_path; *p; p++) if (*p == '\\') *p = '/';

    int fd = _open(unix_path, _O_RDONLY);
    if (fd < 0) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    struct dstorage_file *file = calloc(1, sizeof(*file));
    if (!file) { _close(fd); return E_OUTOFMEMORY; }
    file->lpVtbl = &file_vtbl;
    file->refcount = 1;
    file->fd = fd;
    wcsncpy(file->path, path, 260);
    *ppv = file;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory_CreateStatusArray(
    IDStorageFactory *iface, UINT32 capacity,
    PCSTR name, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (capacity == 0 || capacity > 0x2000) return E_INVALIDARG;
    struct dstorage_status_array *array = calloc(1, sizeof(*array));
    if (!array) return E_OUTOFMEMORY;
    array->lpVtbl = &status_vtbl;
    array->refcount = 1;
    array->capacity = capacity;
    array->slots = calloc(capacity, sizeof(int32_t));
    if (!array->slots) { free(array); return E_OUTOFMEMORY; }
    *ppv = array;
    return S_OK;
}
static void STDMETHODCALLTYPE factory_SetDebugFlags(IDStorageFactory *iface, UINT32 flags) {}
static HRESULT STDMETHODCALLTYPE factory_SetStagingBufferSize(IDStorageFactory *iface, UINT32 size) { return S_OK; }

static const struct IDStorageFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_CreateQueue, factory_OpenFile,
    factory_CreateStatusArray, factory_SetDebugFlags, factory_SetStagingBufferSize
};


/* ==================================================================
 * I/O Completion Thread
 *
 * PR3: Completion path now calls vkd3d-proton for GPU decompression
 * and fence signaling instead of the old libds_gpu.so path.
 * ================================================================== */
struct completion_data
{
    struct dstorage_queue *queue;
    UINT64 request_id;
    struct command_slot *slot;
    ID3D12Resource *dest_resource;
    uint64_t dest_offset;
    uint8_t compression_format;
    void *io_buffer;
    size_t io_size;
    struct dstorage_status_array *status_array;
    UINT32 status_index;
    ID3D12Fence *signal_fence;
    uint64_t signal_value;
    HANDLE signal_event;
};

static void io_completion_callback(void *userdata, int result, unsigned bytes)
{
    struct completion_data *comp = (struct completion_data*)userdata;
    struct dstorage_queue *queue;
    HRESULT status = S_OK;

    if (!comp) return;
    queue = comp->queue;

    if (result < 0)
    {
        status = HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        EnterCriticalSection(&queue->error_cs);
        queue->has_error = TRUE;
        queue->error_record.FailureCount = 1;
        queue->error_record.FirstFailure.HResult = status;
        SetEvent(queue->error_event);
        LeaveCriticalSection(&queue->error_cs);
    }
    else
    {
        /* I/O succeeded. Handle decompression (CPU or GPU path). */
        if (comp->compression_format == 1) /* GDEFLATE */
        {
            /* PR3: Future — dispatch GDeflate compute shader via vkd3d-proton.
             *
             * When GPU decompression is available:
             *   1. p_vkd3d_get_vk_buffer(comp->dest_resource, &vk_buf, &va, &size)
             *   2. Allocate staging VkBuffer + export dma-buf via p_vkd3d_export_dma_buf
             *   3. Dispatch cs_gdeflate.comp via p_vkd3d_submit_compute
             *   4. Signal fence via p_vkd3d_signal_fence
             *
             * Until then, CPU fallback:
             */
        }
    }

    /* PR3: Signal fence via vkd3d-proton (instead of legacy libds_gpu path) */
    if (comp->signal_fence && queue->desc.Device)
    {
        /* The completion thread signals the timeline semaphore directly */
        p_vkd3d_signal_fence(queue->desc.Device, comp->signal_fence, comp->signal_value);
        ID3D12Fence_Release(comp->signal_fence);
    }

    if (comp->signal_event) SetEvent(comp->signal_event);
    if (comp->status_array)
        dstorage_status_array_set(comp->status_array, comp->status_index, status);
    if (comp->io_buffer) free(comp->io_buffer);
    if (comp->dest_resource) ID3D12Resource_Release(comp->dest_resource);
    free(comp);
}

static DWORD WINAPI completion_thread_proc(LPVOID param)
{
    struct dstorage_factory *factory = (struct dstorage_factory*)param;
    while (!factory->stop_completion_thread)
    {
        if (factory->uring_ring && p_ds_uring_drain)
            p_ds_uring_drain(factory->uring_ring);
        else
            Sleep(1);
    }
    return 0;
}

static BOOL start_completion_thread(struct dstorage_factory *factory)
{
    if (factory->completion_thread) return TRUE;
    factory->stop_completion_thread = FALSE;
    factory->completion_thread = CreateThread(NULL, 0, completion_thread_proc, factory, 0, NULL);
    return factory->completion_thread != NULL;
}

static void stop_completion_thread(struct dstorage_factory *factory)
{
    if (!factory->completion_thread) return;
    factory->stop_completion_thread = TRUE;
    WaitForSingleObject(factory->completion_thread, 5000);
    CloseHandle(factory->completion_thread);
    factory->completion_thread = NULL;
}


/* ==================================================================
 * Exported API (dstoragecore.dll entry points)
 * ================================================================== */

HRESULT WINAPI DStorageGetFactoryCore(REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;

    if (!g_factory)
    {
        static CRITICAL_SECTION init_cs;
        static BOOL init_cs_created = FALSE;
        if (!init_cs_created) { InitializeCriticalSection(&init_cs); init_cs_created = TRUE; }

        EnterCriticalSection(&init_cs);
        if (!g_factory)
        {
            struct dstorage_factory *factory = calloc(1, sizeof(*factory));
            if (factory)
            {
                factory->lpVtbl = &factory_vtbl;
                factory->refcount = 1;
                InitializeCriticalSection(&factory->cs);
                load_unix_libraries(factory);
                if (factory->uring_dll && p_ds_uring_init)
                    factory->uring_ring = p_ds_uring_init(1024, 0, 0);
                start_completion_thread(factory);
                g_factory = factory;
            }
        }
        LeaveCriticalSection(&init_cs);
    }

    if (!g_factory) return E_OUTOFMEMORY;
    IDStorageFactory_AddRef((IDStorageFactory*)g_factory);
    *ppv = g_factory;
    return S_OK;
}

HRESULT WINAPI DStorageSetConfigurationCore(const DSTORAGE_CONFIGURATION *configuration)
{
    if (!configuration) return E_INVALIDARG;
    if (!g_factory) return S_OK;
    EnterCriticalSection(&g_factory->cs);
    if (g_factory->queue_count > 0 || g_factory->file_count > 0)
    { LeaveCriticalSection(&g_factory->cs); return STG_E_INVALIDPARAMETER; }
    g_factory->config = *configuration;
    LeaveCriticalSection(&g_factory->cs);
    return S_OK;
}

HRESULT WINAPI DStorageSetConfiguration1Core(const DSTORAGE_CONFIGURATION1 *configuration)
{
    if (!configuration) return E_INVALIDARG;
    return S_OK;
}

HRESULT WINAPI DStorageCreateCompressionCodecCore(
    DSTORAGE_COMPRESSION_FORMAT format, UINT32 numThreads,
    REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (format != 1) return E_INVALIDARG; /* GDeflate only */
    struct dstorage_compression_codec *codec = calloc(1, sizeof(*codec));
    if (!codec) return E_OUTOFMEMORY;
    codec->lpVtbl = &codec_vtbl;
    codec->refcount = 1;
    codec->format = format;
    *ppv = codec;
    return S_OK;
}


/* ==================================================================
 * DLL Entry Point
 * ================================================================== */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        break;
    case DLL_PROCESS_DETACH:
        if (!lpvReserved && g_factory)
            factory_Release((IDStorageFactory*)g_factory);
        break;
    }
    return TRUE;
}
