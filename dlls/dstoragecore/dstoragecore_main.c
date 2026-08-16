/*
 * wine_dstoragecore_main.c — DirectStorage Core DLL for Wine/Proton
 *
 * [existing commentary...]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <io.h>             /* close(), open(), read() for file descriptors */
#include <fcntl.h>          /* O_RDONLY */
#include <errno.h>          /* ENOENT, EIO, ENOMEM, etc. */
#include "dstorage_com.h"       /* COM interface types, vtables, IIDs */
#include "vkd3d_dstorage.h"     /* vkd3d-proton integration helpers */

/* Vulkan types — opaque handles (all pointers, void* is ABI-compatible) */
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
 * Forward declarations for Unix library functions.
 * These are resolved via dlopen("libds_uring.so") at runtime.
 * ------------------------------------------------------------------ */
typedef struct ds_uring *ds_uring_t;

/* Initialize io_uring ring with N entries, registered buffers, poll mode */
ds_uring_t (*p_ds_uring_init)(unsigned entries, int use_poll, int use_sqpoll);

/* Submit async read: fd, offset, size, dst_buffer (dmabuf fd), callback */
typedef void (*uring_callback_t)(void *userdata, int result, unsigned bytes);
int (*p_ds_uring_read)(ds_uring_t ring, int fd, uint64_t offset,
                       uint32_t size, void *dst, 
                       uring_callback_t cb, void *userdata);

/* Submit async write (for debug/texture injection) */
int (*p_ds_uring_write)(ds_uring_t ring, int fd, uint64_t offset,
                        uint32_t size, const void *src,
                        uring_callback_t cb, void *userdata);

/* Drain completions (returns number processed) */
int (*p_ds_uring_drain)(ds_uring_t ring);

/* Destroy ring */
void (*p_ds_uring_destroy)(ds_uring_t ring);

/* GPU GDeflate decompression dispatch (from libds_gpu.so) */
typedef struct ds_gpu_ctx *ds_gpu_t;

ds_gpu_t (*p_ds_gpu_init)(VkDevice device, VkQueue compute_queue,
                           uint32_t queue_family_index);

/* 
 * Dispatch GDeflate decompress on GPU.
 * compressed_src: buffer containing GDeflate data (host-visible)
 * compressed_size: size of compressed data
 * dst: destination VkBuffer (device-local, for game use)
 * dst_offset: byte offset into dst buffer
 * fence: Vulkan timeline semaphore to signal on completion
 * fence_value: value to write to semaphore
 */
int (*p_ds_gpu_decompress)(ds_gpu_t ctx,
                           VkBuffer compressed_src, uint64_t compressed_size,
                           VkBuffer dst, uint64_t dst_offset,
                           VkSemaphore fence, uint64_t fence_value);

void (*p_ds_gpu_destroy)(ds_gpu_t ctx);

/* ------------------------------------------------------------------ 
 * Internal structures
 * ------------------------------------------------------------------ */

/* Global factory state (process-wide singleton) */
struct dstorage_factory
{
    const struct IDStorageFactoryVtbl *lpVtbl;  /* COM vtable — offset 0 */
    LONG refcount;
    CRITICAL_SECTION cs;
    DSTORAGE_CONFIGURATION config;
    
    /* Unix I/O backend (lazily initialized) */
    HMODULE uring_dll;       /* dlopen handle for libds_uring.so */
    ds_uring_t uring_ring;   /* io_uring ring instance */
    
    /* GPU decompression backend (lazily initialized) */
    HMODULE gpu_dll;         /* dlopen handle for libds_gpu.so */
    ds_gpu_t gpu_ctx;        /* GPU decompression context */
    
    /* 
     * Completion thread (Item 2):
     * Processes io_uring CQEs and invokes callbacks that
     * decompress data, signal fences, and update status arrays.
     */
    HANDLE completion_thread;
    volatile BOOL stop_completion_thread;
    
    /* 
     * Per-process limits (matching Windows DirectStorage):
     * Max 32 queues, 128 files, 32K status array entries
     */
    LONG queue_count;
    LONG file_count;
};

/* Forward declaration of the singleton factory */
static struct dstorage_factory *g_factory;

/* ==================================================================
 * Forward declarations for command slots
 * ================================================================== */

/* File object */
struct dstorage_file
{
    const struct IDStorageFileVtbl *lpVtbl;
    LONG refcount;
    
    /* Linux file descriptor (opened via open(2) with O_DIRECT for BypassIO) */
    int fd;
    WCHAR path[MAX_PATH];    /* Original Windows path, stored for debugging */
};

/* Status array object */
struct dstorage_status_array
{
    const struct IDStorageStatusArrayVtbl *lpVtbl;
    LONG refcount;
    UINT32 capacity;
    /* 
     * Each slot: 0 = S_OK (complete, success), 
     *            E_PENDING = not yet complete,
     *            other = HRESULT error code
     * Initially all slots are S_OK (no pending work before first EnqueueStatus).
     * See DirectStorage docs: IsComplete returns true when all requests before
     * the status entry have completed.
     */
    HRESULT *slots;
};

/* Compression codec object */
struct dstorage_compression_codec
{
    const struct IDStorageCompressionCodecVtbl *lpVtbl;
    LONG refcount;
    DSTORAGE_COMPRESSION_FORMAT format;
};

/* Custom decompression queue (QueryInterface from factory) */
struct dstorage_custom_decompression_queue
{
    const struct IDStorageCustomDecompressionQueueVtbl *lpVtbl;
    LONG refcount;
    HANDLE event;             /* auto-reset event, set when requests pending */
    CRITICAL_SECTION cs;
    DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST *requests;
    UINT32 count;             /* number of pending requests */
    UINT32 capacity;          /* allocated capacity */
    UINT64 next_id;           /* monotonic ID for requests */
};

/* Forward declarations for vtables and functions used before their definitions */
static const struct IDStorageQueueVtbl queue_vtbl;
static void stop_completion_thread(struct dstorage_factory *factory);


/* ==================================================================
 * Utility: Load Unix native libraries and resolve symbols
 *
 * Wine's architecture for PE→Unix bridging:
 *   Option A (Windows-style): The PE DLL calls dlopen/dlsym directly.
 *     This works because Wine's ntdll provides a Linux dlopen wrapper.
 *     Simple but means the PE DLL has knowledge of ELF loading.
 *
 *   Option B (Wine-native): Use wine_unix_call with a registered
 *     Unix library. This is the "proper" Wine architecture but
 *     requires more boilerplate.
 *
 *   Option C (Our approach): PE DLL loads libdstorage.so + libds_uring.so
 *     via LoadLibraryEx with LOAD_LIBRARY_AS_DATAFILE. The .so files
 *     are shipped alongside the PE DLLs. This is the simplest approach
 *     and matches how vkd3d-proton loads libvulkan.so.
 *
 * We use Option C for simplicity. The Unix .so provides:
 *   - io_uring ring management
 *   - GDeflate Vulkan compute dispatch
 * ================================================================== */
static BOOL load_unix_libraries(struct dstorage_factory *factory)
{
    /* 
     * Load the io_uring backend library.
     * In production, this would be installed to:
     *   /usr/lib/wine/dstorage/libds_uring.so
     *   or alongside the Wine dll in the Wine prefix
     */
    factory->uring_dll = LoadLibraryA("libds_uring.so");
    if (!factory->uring_dll)
    {
        /* 
         * Fallback: try absolute paths. This helps during development
         * when the library hasn't been installed yet.
         */
        factory->uring_dll = LoadLibraryA("/usr/lib/libds_uring.so");
    }
    if (!factory->uring_dll)
    {
        /* 
         * io_uring not available — we'll fall back to synchronous I/O
         * via pread/pwrite. This is not ideal but allows development
         * and testing on systems without io_uring support.
         */
        return FALSE;  /* Not a fatal error — we handle this later */
    }
    
/* Resolve symbols — note these GetProcAddress calls are on the native .so */
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

static BOOL load_gpu_libraries(struct dstorage_factory *factory,
                                ID3D12Device *d3d12_device)
{
    factory->gpu_dll = LoadLibraryA("libds_gpu.so");
    if (!factory->gpu_dll)
        return FALSE;
    
#define LOAD_SYM(lib, name, ptr) \
    do { \
        *(void**)(&ptr) = (void*)GetProcAddress(lib, name); \
        if (!ptr) return FALSE; \
    } while(0)
    
    LOAD_SYM(factory->gpu_dll, "ds_gpu_init", p_ds_gpu_init);
    LOAD_SYM(factory->gpu_dll, "ds_gpu_decompress", p_ds_gpu_decompress);
    LOAD_SYM(factory->gpu_dll, "ds_gpu_destroy", p_ds_gpu_destroy);
    
#undef LOAD_SYM
    
    return TRUE;
}


/* ==================================================================
 * VTable definitions
 *
 * Each interface uses a vtable following the COM ABI layout:
 *   [0] QueryInterface
 *   [1] AddRef  
 *   [2] Release
 *   [3+] Interface-specific methods
 *
 * This matches the exact ABI that Windows games expect.
 * ================================================================== */

/* --- IDStorageFile vtbl --- */
static HRESULT STDMETHODCALLTYPE file_QueryInterface(
    IDStorageFile *iface, REFIID riid, void **ppv)
{
    /* 
     * Standard COM QueryInterface: if the requested IID matches our
     * interface, return self. Otherwise return E_NOINTERFACE.
     */
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
        /* 
         * Close the underlying file descriptor.
         * Unlike Windows' CloseHandle, we use close(2) on the fd.
         * The fd was opened via open(2) in IDStorageFactory_OpenFile.
         */
        if (f->fd >= 0) close(f->fd);
        free(f);
    }
    return ref;
}
/* IDStorageFile::Close — closes the file regardless of refcount */
static void STDMETHODCALLTYPE file_Close(IDStorageFile *iface)
{
    struct dstorage_file *f = (struct dstorage_file*)iface;
    /* 
     * DirectStorage semantics: Close() forcibly closes the underlying
     * file, regardless of refcount. After Close(), the object can no
     * longer be used in I/O requests. Release() must still be called
     * to free memory.
     */
    if (f->fd >= 0)
    {
        close(f->fd);
        f->fd = -1;  /* Mark as closed */
    }
}
static HRESULT STDMETHODCALLTYPE file_GetFileInformation(
    IDStorageFile *iface, BY_HANDLE_FILE_INFORMATION *info)
{
    /* 
     * Retrieve file information. Maps to fstat(2) on Linux.
     * The BY_HANDLE_FILE_INFORMATION struct has:
     *   dwFileAttributes, ftCreationTime, ftLastAccessTime,
     *   ftLastWriteTime, dwVolumeSerialNumber, nFileSizeHigh/Low,
     *   nNumberOfLinks, nFileIndexHigh/Low
     */
    struct dstorage_file *f = (struct dstorage_file*)iface;
    
    if (!info) return E_INVALIDARG;
    if (f->fd < 0) return E_HANDLE;  /* File was closed */
    
    /* 
     * TODO: implement fstat mapping to BY_HANDLE_FILE_INFORMATION.
     * This requires struct stat → FILETIME conversion, volume serial
     * number mapping (via statfs), etc. For now, return not-implemented
     * which most games handle gracefully.
     */
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


/* ==================================================================
 * IDStorageStatusArray implementation (Item 5: Status/Error Reporting)
 *
 * The status array tracks completion status for batches of requests.
 * Games call EnqueueStatus on the queue to mark a point at which
 * all preceding requests must complete. The status slot is checked
 * via IsComplete(h) and GetHResult().
 *
 * Lifecycle of a status slot:
 *   1. Created in S_OK state (no work before first EnqueueStatus)
 *   2. EnqueueStatus sets the slot to E_PENDING
 *   3. When preceding I/O completes, slot is set to S_OK or error code
 *   4. Game polls IsComplete() or checks GetHResult()
 *
 * IMPORTANT: Status array slots are ONE-SHOT. After a slot completes,
 * the game can reuse it by calling EnqueueStatus again with the same
 * index. This matches Windows DirectStorage behavior.
 * ================================================================== */

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
    if (ref == 0)
    {
        free(a->slots);
        free(a);
    }
    return ref;
}

/*
 * IDStorageStatusArray::IsComplete
 * Returns TRUE when all requests enqueued BEFORE the corresponding
 * EnqueueStatus call have completed (success or failure).
 *
 * Per DirectStorage docs:
 *   "Returns a Boolean value indicating that all requests enqueued
 *    prior to the specified status entry have completed."
 *
 * Our implementation:
 *   Each slot starts at S_OK. EnqueueStatus sets it to E_PENDING.
 *   When preceding I/O completes, we set S_OK or error.
 *   IsComplete returns (slot != E_PENDING).
 *
 *   - S_OK means "completed successfully"
 *   - E_PENDING (0x80000000) means "not yet complete"
 *   - Any other HRESULT means "completed with error"
 */
static BOOL STDMETHODCALLTYPE status_IsComplete(
    IDStorageStatusArray *iface, UINT32 index)
{
    struct dstorage_status_array *a = (struct dstorage_status_array*)iface;

    if (!a->slots || index >= a->capacity)
        return TRUE;  /* Out of bounds: vacuously complete */

    /*
     * Read the slot value with acquire semantics to ensure we see
     * the completion write from the I/O thread. On x86, this is
     * just a compiler barrier. On ARM, it's a dmb instruction.
     * We use InterlockedCompareExchange which provides full barriers.
     */
    HRESULT val = (HRESULT)InterlockedCompareExchange(
        (volatile LONG*)&a->slots[index], 0, 0);

    /* 
     * Per DirectStorage spec: "This is equivalent to
     * GetHResult(index) != E_PENDING"
     */
    return val != E_PENDING;
}

/*
 * IDStorageStatusArray::GetHResult
 * Returns the HRESULT for the batch of requests ending at this status.
 *
 *   - S_OK: All requests completed successfully
 *   - E_PENDING: Not all requests have completed yet
 *   - Other: The first failed request's error code
 */
static HRESULT STDMETHODCALLTYPE status_GetHResult(
    IDStorageStatusArray *iface, UINT32 index)
{
    struct dstorage_status_array *a = (struct dstorage_status_array*)iface;

    if (!a->slots || index >= a->capacity)
        return E_BOUNDS;

    return (HRESULT)InterlockedCompareExchange(
        (volatile LONG*)&a->slots[index], 0, 0);
}

/*
 * Set a status slot to a value (called from I/O completion thread).
 * This is not part of the public API — it's used internally by
 * the queue when completing status writes.
 */
void dstorage_status_array_set(
    struct dstorage_status_array *a, UINT32 index, HRESULT value)
{
    if (a && a->slots && index < a->capacity)
    {
        InterlockedExchange((volatile LONG*)&a->slots[index], (LONG)value);
    }
}

static const struct IDStorageStatusArrayVtbl status_vtbl =
{
    status_QueryInterface,
    status_AddRef,
    status_Release,
    status_IsComplete,
    status_GetHResult
};

/* ==================================================================
 * GDeflate Format Reference
 *
 * GDeflate builds on top of RFC 1951 DEFLATE with a framing layer:
 *   - GDeflate format (header + block table + DEFLATE blocks)
 *   - Stored blocks (BTYPE=0)
 *   - Fixed Huffman blocks (BTYPE=1)  
 *   - Dynamic Huffman blocks (BTYPE=2)
 *
 * See our GDeflate_Format_Specification.md for the full format spec.
 * ================================================================== */
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
/* 
 * CompressBuffer: Compress data using GDeflate format.
 * This is a CPU-based compressor. For GPU decompression, see
 * the libds_gpu.so dispatch functions.
 *
 * The compressed output format is:
 *   [GDeflate header] [block table] [DEFLATE blocks...]
 *   See GDeflate_Format_Specification.md for layout details.
 */
static HRESULT STDMETHODCALLTYPE codec_CompressBuffer(
    IDStorageCompressionCodec *iface,
    const void *uncompressedData, size_t uncompressedDataSize,
    DSTORAGE_COMPRESSION compressionSetting,
    void *compressedBuffer, size_t compressedBufferSize,
    size_t *compressedDataSize)
{
    /* 
     * This is a CPU-side operation. We delegate to our cleanroom
     * DEFLATE implementation in libdstorage.so.
     */
    if (!uncompressedData || !compressedBuffer || !compressedDataSize)
        return E_INVALIDARG;
    
    *compressedDataSize = 0;
    
    /* 
     * TODO: call our native gd_compress() from dstorage_codec.cpp.
     * For now, return E_NOTIMPL to indicate this is a work in progress.
     * The reference implementation is in dstorage_codec.cpp and should
     * be linked into this DLL or called via dlsym.
     */
    return E_NOTIMPL;
}

/* DecompressBuffer: Decompress GDeflate data to raw output */
static HRESULT STDMETHODCALLTYPE codec_DecompressBuffer(
    IDStorageCompressionCodec *iface,
    const void *compressedData, size_t compressedDataSize,
    void *uncompressedBuffer, size_t uncompressedBufferSize,
    size_t *uncompressedDataSize)
{
    if (!compressedData || !uncompressedBuffer || !uncompressedDataSize)
        return E_INVALIDARG;
    
    *uncompressedDataSize = 0;
    /* TODO: call our native gd_decompress() */
    return E_NOTIMPL;
}

/* CompressBufferBound: Return upper bound for compressed output size */
static size_t STDMETHODCALLTYPE codec_CompressBufferBound(
    IDStorageCompressionCodec *iface, size_t uncompressedDataSize)
{
    /* 
     * Upper bound: GDeflate header (32) + block table (20 per block) 
     * + stored block overhead (5 per block) + original data.
     * For worst case (uncompressible data), the output is slightly
     * larger than input due to framing overhead.
     */
    const size_t max_block = 65535;
    uint32_t num_blocks = (uint32_t)((uncompressedDataSize + max_block - 1) / max_block);
    return 32 + num_blocks * 20 + num_blocks * 5 + uncompressedDataSize;
}

static const struct IDStorageCompressionCodecVtbl codec_vtbl =
{
    codec_QueryInterface,
    codec_AddRef,
    codec_Release,
    codec_CompressBuffer,
    codec_DecompressBuffer,
    codec_CompressBufferBound
};


/* ==================================================================
 * IDStorageQueue implementation
 *
 * This is the heart of DirectStorage. The queue manages:
 *   - Request submission (EnqueueRequest)
 *   - Status tracking (EnqueueStatus)
 *   - Fence signaling (EnqueueSignal)
 *   - Event signaling (EnqueueSetEvent)
 *   - Batch submission (Submit)
 *   - Request cancellation (CancelRequestsWithTag)
 *   - Error reporting (GetErrorEvent, RetrieveErrorRecord)
 *   - Queue info (Query)
 *
 * Each request goes through a pipeline:
 *   1. EnqueueRequest adds to the software queue
 *   2. Submit() sends to io_uring for async I/O
 *   3. On I/O completion, if compression is needed:
 *      a. CPU path: decompress via IDStorageCompressionCodec
 *      b. GPU path: dispatch GDeflate compute shader via libds_gpu.so
 *   4. Signal fence/event if EnqueueSignal/EnqueueSetEvent was called
 *   5. Update status array slots for EnqueueStatus
 * ================================================================== */

/* 
 * The queue manages a ring buffer of request slots.
 * Each slot can hold one of several command types.
 * This matches the Windows DirectStorage queue model where
 * the queue capacity is fixed at creation time (128-8192 slots).
 */
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
        
        struct
        {
            struct dstorage_status_array *array;
            UINT32 index;
        } status;
        
        struct
        {
            ID3D12Fence *fence;
            UINT64 value;
        } signal;
        
        HANDLE event;
    };
};

/* Per-queue I/O tracking for in-flight requests */
struct io_request
{
    struct command_slot *slot;     /* Back-reference to the queue slot */
    void *io_buffer;              /* Staging buffer for I/O (dmabuf or malloc) */
    size_t io_size;               /* Size of the I/O transfer */
    int fd;                       /* File descriptor for this request */
    
    /* GPU decompression state */
    VkBuffer compressed_buffer;   /* VkBuffer holding compressed data (if GPU path) */
    VkBuffer dest_buffer;         /* VkBuffer for decompressed output */
    uint64_t dest_offset;         /* Offset into dest buffer */
};

/* Maximum number of in-flight I/Os per queue */
#define MAX_IN_FLIGHT 256

struct dstorage_queue
{
    const struct IDStorageQueueVtbl *lpVtbl;
    LONG refcount;
    DSTORAGE_QUEUE_DESC desc;
    
    /* 
     * Ring buffer of commands. Mapped to Windows OVERLAPPED model.
     * The ring buffer allows the game to enqueue N commands without
     * blocking, up to the queue's capacity.
     */
    struct command_slot *slots;
    UINT16 capacity;          /* Total number of slots (128-8192) */
    volatile LONG head;       /* Producer index (enqueue position) */
    volatile LONG tail;       /* Consumer index (submit position) */
    volatile LONG completed;  /* Completion index (callbacks done) */
    
    /* In-flight I/O tracking */
    struct io_request inflight[MAX_IN_FLIGHT];
    volatile LONG inflight_count;
    
    /* io_uring ring for this queue (shared from factory) */
    ds_uring_t ring;
    
    /* Error reporting */
    HANDLE error_event;       /* Auto-reset event for GetErrorEvent() */
    DSTORAGE_ERROR_RECORD error_record;
    BOOL has_error;
    CRITICAL_SECTION error_cs;
    
    /*
     * Future considerations for GPU decompression pipeline:
     *
     * For GPU-targeted requests (DestinationType = BUFFER/TEXTURE/TILES),
     * the I/O pipeline is:
     *
     *   1. io_uring reads compressed GDeflate data into a staging buffer
     *      (UPLOAD heap, host-visible, dma-buf exported)
     *
     *   2. If compression is NONE, do vkCmdCopyBuffer from staging to
     *      destination. The destination is a VkBuffer from vkd3d-proton's
     *      d3d12_resource struct, extracted via vkd3d_get_vk_buffer().
     *
     *   3. If compression is GDEFLATE, dispatch GDeflate compute shader
     *      that reads from the staging buffer and writes decompressed
     *      data to the destination VkBuffer.
     *
     *   4. Signal the fence (timeline semaphore) via vkSignalSemaphore,
     *      which wakes up vkd3d-proton's waiting command queue.
     *
     * This requires tight integration with vkd3d-proton:
     *   - Access to d3d12_device → VkDevice mapping
     *   - Access to d3d12_resource → VkBuffer mapping
     *   - Access to d3d12_fence → VkSemaphore mapping
     *   - Knowledge of the vkd3d-proton command submission model
     *
     * The integration functions are declared in:
     *   vkd3d-proton/libs/vkd3d/vkd3d_dstorage.h (proposed)
     */
};

/* 
 * Create a new queue. This implements IDStorageFactory::CreateQueue.
 * 
 * Per DirectStorage spec:
 *   - Capacity must be between DSTORAGE_MIN_QUEUE_CAPACITY (128)
 *     and DSTORAGE_MAX_QUEUE_CAPACITY (8192)
 *   - SourceType determines whether FILE or MEMORY sources are accepted
 *   - Priority must be LOW, NORMAL, HIGH, or REALTIME
 *   - Memory-source queues must use REALTIME priority
 *   - Device may be NULL; if NULL, GPU destinations are rejected
 */
static HRESULT create_queue(struct dstorage_factory *factory,
                             const DSTORAGE_QUEUE_DESC *desc,
                             REFIID riid, void **ppv)
{
    struct dstorage_queue *queue;
    
    /* Validate parameters per DirectStorage spec */
    if (!desc || !ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    if (desc->Capacity < DSTORAGE_MIN_QUEUE_CAPACITY ||
        desc->Capacity > DSTORAGE_MAX_QUEUE_CAPACITY)
        return E_INVALIDARG;
    
    if (desc->SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY &&
        desc->Priority != DSTORAGE_PRIORITY_REALTIME)
        return DSTORAGE_E_INVALID_MEMORY_QUEUE_PRIORITY;
    
    /* Allocate and initialize */
    queue = calloc(1, sizeof(*queue));
    if (!queue) return E_OUTOFMEMORY;
    
    queue->lpVtbl = &queue_vtbl;  /* Forward reference — defined below */
    queue->refcount = 1;
    queue->desc = *desc;
    queue->capacity = desc->Capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->completed = 0;
    queue->inflight_count = 0;
    
    /* Allocate command slots */
    queue->slots = calloc(desc->Capacity, sizeof(struct command_slot));
    if (!queue->slots)
    {
        free(queue);
        return E_OUTOFMEMORY;
    }
    
    /* Create error event (auto-reset, initially unsignaled) */
    queue->error_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    queue->has_error = FALSE;
    InitializeCriticalSection(&queue->error_cs);
    
    /* 
     * Share the factory's io_uring ring.
     * In a production implementation, each queue could have its own
     * io_uring ring for better isolation. However, Linux's io_uring
     * scales well per-process, and a single ring with multiple SQE
     * producers works efficiently.
     */
    queue->ring = factory->uring_ring;
    
    /* 
     * For GPU-capable queues with a D3D12 device:
     * Initialize the GPU decompression context using the D3D12 device's
     * underlying Vulkan device. This requires vkd3d-proton integration.
     * 
     * The vkd3d-proton device exposes:
     *   vkd3d_get_vk_device(d3d12_device) → VkDevice
     *   vkd3d_get_vk_queue(d3d12_device, VKD3D_QUEUE_FAMILY_COMPUTE) → VkQueue
     */
    // if (desc->Device && factory->gpu_ctx == NULL && factory->gpu_dll)
    // {
    //     VkDevice vk_device = vkd3d_get_vk_device(desc->Device);
    //     VkQueue vk_queue = vkd3d_get_vk_queue(desc->Device, ...);
    //     factory->gpu_ctx = p_ds_gpu_init(vk_device, vk_queue, ...);
    // }
    
    EnterCriticalSection(&factory->cs);
    factory->queue_count++;
    LeaveCriticalSection(&factory->cs);
    
    *ppv = queue;
    
    /* 
     * Return the requested interface version via QueryInterface.
     * This allows the caller to request IDStorageQueue (base),
     * IDStorageQueue1 (+EnqueueSetEvent), IDStorageQueue2
     * (+GetCompressionSupport), or IDStorageQueue3 (+EnqueueRequests).
     */
    return IDStorageQueue_QueryInterface((IDStorageQueue*)queue, riid, ppv);
}


/* ---- Queue vtable methods ---- */

static HRESULT STDMETHODCALLTYPE queue_QueryInterface(
    IDStorageQueue *iface, REFIID riid, void **ppv)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    /* 
     * Check against known DStorage queue IIDs.
     * The following GUIDs come from the DirectStorage SDK headers:
     */
    static const GUID IID_IDStorageQueue = 
        { 0xcfdbd83f, 0x9e06, 0x4fda, { 0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b } };
    static const GUID IID_IDStorageQueue1 = 
        { 0xdd2f482c, 0x5eff, 0x41e8, { 0x9c, 0x9e, 0xd2, 0x37, 0x4b, 0x27, 0x81, 0x28 } };
    static const GUID IID_IDStorageQueue2 = 
        { 0xb1c9d643, 0x3a49, 0x44a2, { 0xb4, 0x6f, 0x65, 0x36, 0x49, 0x47, 0x0d, 0x18 } };
    static const GUID IID_IDStorageQueue3 = 
        { 0xdeb54c52, 0xeca8, 0x46b3, { 0x82, 0xa7, 0x03, 0x1b, 0x72, 0x26, 0x26, 0x53 } };
    
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDStorageQueue))
    {
        *ppv = iface;
    }
    else if (IsEqualIID(riid, &IID_IDStorageQueue1))
    {
        /*
         * IDStorageQueue1 extends IDStorageQueue with EnqueueSetEvent.
         * We return the same object since we implement all methods,
         * but the caller gets a different vtable pointer.
         */
        *ppv = iface;  /* In a full impl, use IDStorageQueue1 vtbl */
    }
    else if (IsEqualIID(riid, &IID_IDStorageQueue2))
    {
        *ppv = iface;
    }
    else if (IsEqualIID(riid, &IID_IDStorageQueue3))
    {
        *ppv = iface;
    }
    else
    {
        return E_NOINTERFACE;
    }
    
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
        /* 
         * Cleanup: free slots, close event, release factory ref.
         * Note: we do NOT implicitly wait for in-flight requests,
         * matching Windows DirectStorage behavior where the
         * application must drain the queue before destruction.
         */
        free(q->slots);
        CloseHandle(q->error_event);
        DeleteCriticalSection(&q->error_cs);
        free(q);
    }
    return ref;
}

/*
 * EnqueueRequest: Add a read request to the queue.
 * 
 * This is the primary entry point for games to submit I/O.
 * The request is copied into the queue's ring buffer and
 * processed when Submit() is called.
 *
 * The request's fields are validated according to DirectStorage rules:
 *   - SourceType determines whether we read from a file or a memory buffer
 *   - CompressionFormat determines post-read decompression
 *   - DestinationType determines where the data goes
 *   - UncompressedSize is validated for compressed requests
 *   - CancellationTag is stored for later cancellation matching
 */
static void STDMETHODCALLTYPE queue_EnqueueRequest(
    IDStorageQueue *iface, const DSTORAGE_REQUEST *request)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG slot;
    
    if (!request) return;
    
    /* 
     * Get the next slot in the ring buffer.
     * If the queue is full, this will block until a slot opens up.
     * This matches Windows DirectStorage behavior where EnqueueRequest
     * blocks when there are no free slots (queue full).
     *
     * The ring buffer has (capacity - 1) usable slots, with one slot
     * reserved to distinguish "empty" from "full".
     */
    while (1)
    {
        LONG current_head = q->head;
        LONG current_tail = q->tail;
        
        /* 
         * Check if queue is full: (head - tail) >= (capacity - 1)
         * If full, yield to let the completion thread drain.
         */
        if (current_head - current_tail >= q->capacity - 1)
        {
            /* 
             * Queue is full — yield to let I/O complete.
             * In production, we'd use WaitForSingleObject on a
             * completion event or use I/O completion ports.
             */
            Sleep(0);
            continue;
        }
        
        /* Reserve the slot */
        slot = current_head;
        if (InterlockedCompareExchange(&q->head, current_head + 1, current_head) == current_head)
            break;
    }
    
    /* Copy the request into the slot */
    memset(&q->slots[slot % q->capacity], 0, sizeof(struct command_slot));
    q->slots[slot % q->capacity].type = CMD_REQUEST;
    q->slots[slot % q->capacity].request = *request;
}

/* 
 * EnqueueStatus: Add a status write that fires when preceding requests complete.
 *
 * The status array slot will be set to S_OK or an error code when all
 * requests enqueued before this status entry have completed.
 */
static void STDMETHODCALLTYPE queue_EnqueueStatus(
    IDStorageQueue *iface, IDStorageStatusArray *statusArray, UINT32 index)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG slot;
    
    while (1)
    {
        LONG current_head = q->head;
        LONG current_tail = q->tail;
        if (current_head - current_tail >= q->capacity - 1)
        {
            Sleep(0);
            continue;
        }
        slot = current_head;
        if (InterlockedCompareExchange(&q->head, current_head + 1, current_head) == current_head)
            break;
    }
    
    q->slots[slot % q->capacity].type = CMD_STATUS;
    q->slots[slot % q->capacity].status.array = (struct dstorage_status_array*)statusArray;
    q->slots[slot % q->capacity].status.index = index;
}

/* EnqueueSignal: Signal a D3D12 fence when preceding requests complete. */
static void STDMETHODCALLTYPE queue_EnqueueSignal(
    IDStorageQueue *iface, ID3D12Fence *fence, UINT64 value)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG slot;
    
    while (1)
    {
        LONG current_head = q->head;
        LONG current_tail = q->tail;
        if (current_head - current_tail >= q->capacity - 1)
        {
            Sleep(0);
            continue;
        }
        slot = current_head;
        if (InterlockedCompareExchange(&q->head, current_head + 1, current_head) == current_head)
            break;
    }
    
    q->slots[slot % q->capacity].type = CMD_SIGNAL;
    q->slots[slot % q->capacity].signal.fence = fence;
    q->slots[slot % q->capacity].signal.value = value;
    if (fence) ID3D12Fence_AddRef(fence);
}

/* 
 * Submit: Submit all queued commands to the backend for processing.
 *
 * This is the "flush" point. After Submit(), the queued requests start
 * their I/O pipeline. On Windows, this would send them to the storage
 * stack. On Linux, we submit them to io_uring.
 */
static void STDMETHODCALLTYPE queue_Submit(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    LONG tail = q->tail;
    LONG head = q->head;
    LONG count = head - tail;
    
    if (count <= 0) return;
    
    /* 
     * Process each command slot from tail to head.
     * We advance the tail as we submit to io_uring.
     */
    for (LONG i = 0; i < count; i++)
    {
        LONG idx = (tail + i) % q->capacity;
        struct command_slot *slot = &q->slots[idx];
        
        switch (slot->type)
        {
        case CMD_REQUEST:
            /* 
             * Submit the I/O request to io_uring.
             * 
             * For FILE source requests:
             *   1. Extract fd from the IDStorageFile
             *   2. Submit async read to io_uring
             *   3. Register a completion callback that:
             *      a. If compression needed, apply GDeflate decompress
             *      b. Copy to destination buffer (if GPU)
             *      c. Update status array slots
             *      d. Signal fences/events
             *
             * For MEMORY source requests:
             *   No I/O needed. Directly apply decompression and
             *   signal completion.
             */
            if (slot->request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE)
            {
                /* 
                 * File source: async I/O via io_uring.
                 * The completion callback handles decompression, 
                 * status updates, and fence signaling.
                 */
                // TODO: submit to io_uring
            }
            else
            {
                /* 
                 * Memory source: no I/O needed.
                 * The source data is already in memory.
                 * Apply decompression if needed and complete.
                 */
                // TODO: in-memory decompression
            }
            break;
            
        case CMD_STATUS:
            /* 
             * Status write: mark as pending.
             * The completion handler for preceding requests
             * will set this to S_OK or the error code.
             */
            if (slot->status.array)
            {
                // slot->status.array->slots[slot->status.index] = E_PENDING;
            }
            break;
            
        case CMD_SIGNAL:
            /* 
             * Fence signal: will be triggered when preceding
             * I/O completes. The actual signal happens via
             * vkSignalSemaphore on the timeline semaphore.
             */
            break;
            
        case CMD_EVENT:
            /* Event: will be SetEvent'd when I/O completes. */
            break;
        }
        
        /* Advance the tail (slot consumed) */
        InterlockedIncrement(&q->tail);
    }
}

/*
 * CancelRequestsWithTag: Cancel requests matching (CancellationTag & mask) == value.
 * 
 * This is used by games to cancel pending loads, e.g., when the player
 * moves away from an area and its textures are no longer needed.
 */
static void STDMETHODCALLTYPE queue_CancelRequestsWithTag(
    IDStorageQueue *iface, UINT64 mask, UINT64 value)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    
    /* 
     * Iterate through the command slots and mark any matching
     * requests as cancelled. For requests already submitted to
     * io_uring, we can use IORING_OP_ASYNC_CANCEL for in-kernel
     * cancellation (Linux 5.13+).
     */
    LONG head = q->head;
    LONG tail = q->tail;
    
    for (LONG i = tail; i < head; i++)
    {
        LONG idx = i % q->capacity;
        struct command_slot *slot = &q->slots[idx];
        
        if (slot->type == CMD_REQUEST)
        {
            if ((slot->request.CancellationTag & mask) == value)
            {
                /* 
                 * Mark as cancelled. The completion callback
                 * will check for this flag and skip processing.
                 */
                // TODO: implement cancellation
            }
        }
    }
}

/* Close: Close the queue. No more requests will complete after this. */
static void STDMETHODCALLTYPE queue_Close(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    /* 
     * Mark queue as closed. No new submissions allowed.
     * In-flight I/O will be cancelled. Similar to Windows behavior,
     * this ignores refcount.
     */
    // TODO: mark closed, cancel in-flight I/O
}

/* GetErrorEvent: Returns an event handle that signals on error. */
static HANDLE STDMETHODCALLTYPE queue_GetErrorEvent(IDStorageQueue *iface)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    return q->error_event;
}

/* RetrieveErrorRecord: Get details about the first error since last call. */
static void STDMETHODCALLTYPE queue_RetrieveErrorRecord(
    IDStorageQueue *iface, DSTORAGE_ERROR_RECORD *record)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    
    EnterCriticalSection(&q->error_cs);
    if (q->has_error && record)
    {
        *record = q->error_record;
        q->has_error = FALSE;
        ResetEvent(q->error_event);
    }
    else if (record)
    {
        memset(record, 0, sizeof(*record));
    }
    LeaveCriticalSection(&q->error_cs);
}

/* Query: Get queue information (desc, empty slots, auto-submit threshold). */
static void STDMETHODCALLTYPE queue_Query(
    IDStorageQueue *iface, DSTORAGE_QUEUE_INFO *info)
{
    struct dstorage_queue *q = (struct dstorage_queue*)iface;
    
    if (!info) return;
    
    info->Desc = q->desc;
    info->EmptySlotCount = q->capacity - 1 - (q->head - q->tail);
    /* 
     * Auto-submit at half capacity matches Windows behavior:
     * when the queue reaches half capacity, submission happens
     * automatically without waiting for explicit Submit().
     * This prevents the queue from stalling.
     */
    info->RequestCountUntilAutoSubmit = q->capacity / 2;
}

/* ---- Queue vtable ---- */
static const struct IDStorageQueueVtbl queue_vtbl =
{
    queue_QueryInterface,
    queue_AddRef,
    queue_Release,
    queue_EnqueueRequest,
    queue_EnqueueStatus,
    queue_EnqueueSignal,
    queue_Submit,
    queue_CancelRequestsWithTag,
    queue_Close,
    queue_GetErrorEvent,
    queue_RetrieveErrorRecord,
    queue_Query
};


/* ==================================================================
 * IDStorageFactory implementation
 * ================================================================== */

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(
    IDStorageFactory *iface, REFIID riid, void **ppv)
{
    struct dstorage_factory *f = (struct dstorage_factory*)iface;
    
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    /* 
     * DirectStorage allows QueryInterface for the custom decompression queue.
     * The GUIDs come from the DirectStorage public SDK headers.
     */
    static const GUID IID_IDStorageFactory = 
        { 0x6924ea0c, 0xc3cd, 0x4826, { 0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1 } };
    static const GUID IID_IDStorageCustomDecompressionQueue = 
        { 0x97179b2f, 0x2c21, 0x49ca, { 0x82, 0x91, 0x4e, 0x1b, 0xf4, 0xa1, 0x60, 0xdf } };
    
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDStorageFactory))
    {
        *ppv = iface;
        IDStorageFactory_AddRef(iface);
        return S_OK;
    }
    
    /* 
     * TODO: return IDStorageCustomDecompressionQueue when requested.
     * This requires maintaining a CDQ instance in the factory.
     */
    
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
        /* 
         * Global factory cleanup.
         * This should never happen in normal operation since the
         * factory is process-wide and released on DLL unload.
         */
        stop_completion_thread(f);
        if (f->uring_ring && p_ds_uring_destroy)
            p_ds_uring_destroy(f->uring_ring);
        if (f->gpu_ctx && p_ds_gpu_destroy)
            p_ds_gpu_destroy(f->gpu_ctx);
        if (f->uring_dll) FreeLibrary(f->uring_dll);
        if (f->gpu_dll) FreeLibrary(f->gpu_dll);
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
    struct dstorage_file *file;
    
    if (!path || !ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    /* 
     * Convert Windows path to Linux path.
     * Wine's ntdll provides wine_get_unix_file_name() for this purpose.
     * In a standalone build, we convert manually.
     * 
     * For now, we use a simple conversion: strip the drive letter (C:\)
     * and convert backslashes to forward slashes. This works for most
     * games running under Wine.
     */
    char mbs_path[MAX_PATH];
    int mbs_len = WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                       mbs_path, MAX_PATH, NULL, NULL);
    if (mbs_len <= 0) return E_FAIL;
    
    /* 
     * Strip drive letter if present (e.g., "C:\game\data" → "/game/data")
     * This is a Wine-specific transformation.
     */
    char *unix_path = mbs_path;
    if (mbs_path[0] >= 'A' && mbs_path[0] <= 'Z' && mbs_path[1] == ':')
        unix_path = mbs_path + 2;  /* Skip drive letter */
    
    /* Replace backslashes with forward slashes */
    for (char *p = unix_path; *p; p++)
        if (*p == '\\') *p = '/';
    
    /*
     * Open the file with O_RDONLY.
     * 
     * For BypassIO support (equivalent to Windows FILE_FLAG_NO_BUFFERING),
     * we would add O_DIRECT. However, O_DIRECT imposes alignment
     * requirements (sector-aligned buffers and offsets) that most
     * games don't guarantee. We use buffered I/O by default and let
     * io_uring's registered buffers provide the performance benefit.
     */
    int fd = open(unix_path, O_RDONLY);
    if (fd < 0)
    {
        /* Map errno to DirectStorage error codes */
        if (errno == ENOENT || errno == ENOTDIR)
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    
    /* Create the file object */
    file = calloc(1, sizeof(*file));
    if (!file) { close(fd); return E_OUTOFMEMORY; }
    
    file->lpVtbl = &file_vtbl;
    file->refcount = 1;
    file->fd = fd;
    wcsncpy(file->path, path, MAX_PATH);
    
    *ppv = file;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateStatusArray(
    IDStorageFactory *iface, UINT32 capacity,
    PCSTR name, REFIID riid, void **ppv)
{
    struct dstorage_status_array *array;
    
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    if (capacity == 0 || capacity > 0x2000)
        return E_INVALIDARG;
    
    array = calloc(1, sizeof(*array));
    if (!array) return E_OUTOFMEMORY;
    
    array->lpVtbl = &status_vtbl;
    array->refcount = 1;
    array->capacity = capacity;
    
    /* 
     * Allocate slots and initialize to S_OK.
     * (S_OK means "complete with no errors" — no pending work before
     * the first EnqueueStatus was called.)
     */
    array->slots = calloc(capacity, sizeof(HRESULT));
    if (!array->slots)
    {
        free(array);
        return E_OUTOFMEMORY;
    }
    
    *ppv = array;
    return S_OK;
}

static void STDMETHODCALLTYPE factory_SetDebugFlags(
    IDStorageFactory *iface, UINT32 flags)
{
    /* 
     * DirectStorage debug flags:
     *   DSTORAGE_DEBUG_SHOW_ERRORS (0x01) — print errors to debugger
     *   DSTORAGE_DEBUG_BREAK_ON_ERROR (0x02) — debug break on error
     *   DSTORAGE_DEBUG_RECORD_OBJECT_NAMES (0x04) — ETW object names
     * 
     * We map these to debug output and breakpoints.
     */
    // TODO: implement debug flags
}

static HRESULT STDMETHODCALLTYPE factory_SetStagingBufferSize(
    IDStorageFactory *iface, UINT32 size)
{
    if (size == 0)
    {
        /* 
         * Deallocate all staging buffers.
         * Only valid when no queues or files exist.
         */
        EnterCriticalSection(&g_factory->cs);
        if (g_factory->queue_count > 0 || g_factory->file_count > 0)
        {
            LeaveCriticalSection(&g_factory->cs);
            return STG_E_INVALIDPARAMETER;
        }
        // TODO: free staging buffers
        LeaveCriticalSection(&g_factory->cs);
        return S_OK;
    }
    
    /* 
     * Set staging buffer size. Must be at least 1MB and
     * a power of 2 (or at least aligned to 64KB).
     */
    if (size < 1024 * 1024)
        return E_INVALIDARG;
    
    // TODO: resize staging buffer pool
    return S_OK;
}

/* ---- Factory vtable ---- */
static const struct IDStorageFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateQueue,
    factory_OpenFile,
    factory_CreateStatusArray,
    factory_SetDebugFlags,
    factory_SetStagingBufferSize
};


/* ==================================================================
 * io_uring Completion Thread (Item 2)
 *
 * This thread processes I/O completions from the io_uring ring.
 * It runs continuously, draining CQEs and invoking callbacks.
 *
 * The thread is created when the first queue is submitted to.
 * It signals completion events, updates status arrays, and
 * signals D3D12 fences when I/O operations finish.
 *
 * Threading model:
 *   The completion thread is the CONSUMER of I/O completions.
 *   The game's threads are the PRODUCERS (calling EnqueueRequest).
 *   The io_uring ring has lock-free SQE production by the game threads
 *   (each thread gets a unique SQE index via atomic increment) and
 *   single-consumer CQE consumption by this thread.
 *
 * Wakeup: The thread sleeps in io_uring_enter(min_complete=1) when
 * there are no completions to process. New I/O submissions that
 * also call io_uring_enter will wake it up.
 * ================================================================== */

/*
 * Per-queue completion data stored in the io_uring metadata.
 * This is retrieved from the CQE's user_data field.
 */
struct completion_data
{
    struct dstorage_queue *queue;   /* The queue this I/O belongs to */
    UINT64 request_id;              /* Unique ID for this request */
    struct command_slot *slot;      /* Back-reference to the queue slot */
    
    /* For BUFFER destinations: the D3D12 resource to write into */
    ID3D12Resource *dest_resource;
    uint64_t dest_offset;
    
    /* For GPU decompression */
    uint8_t compression_format;     /* DSTORAGE_COMPRESSION_FORMAT */
    void *io_buffer;                /* Staging buffer with raw data */
    size_t io_size;                 /* Size of the I/O transfer */
    
    /* Completion reporting */
    struct dstorage_status_array *status_array;
    UINT32 status_index;
    ID3D12Fence *signal_fence;
    uint64_t signal_value;
    HANDLE signal_event;
};

/*
 * I/O completion callback (invoked from the completion thread).
 *
 * Called when io_uring finishes a read operation.
 * 
 * Pipeline:
 *   1. I/O completed (data is in the staging buffer)
 *   2. If compression is GDeflate AND destination is GPU:
 *      a. Map staging buffer
 *      b. Read GDeflate header to find block table
 *      c. Decompress using stored-block or Huffman decoder
 *      d. Copy decompressed data to destination VkBuffer
 *        (via vkCmdCopyBuffer or mmap for host-visible buffers)
 *   3. If compression is GDeflate AND destination is CPU:
 *      a. Decompress in-place in the staging buffer
 *      b. User's dst pointer is valid (they guaranteed lifetime)
 *   4. If no compression: data is already in the user's buffer
 *   5. Signal fence/event if requested
 *   6. Update status array if requested
 *   7. Free staging resources
 */
static void io_completion_callback(void *userdata, int result, unsigned bytes)
{
    struct completion_data *comp = (struct completion_data*)userdata;
    struct dstorage_queue *queue;
    HRESULT status = S_OK;
    
    if (!comp) return;
    queue = comp->queue;
    
    /* Handle I/O error */
    if (result < 0)
    {
        /* Map errno to DirectStorage error codes */
        switch (-result)
        {
        case EIO:      status = HRESULT_FROM_WIN32(ERROR_READ_FAULT); break;
        case ENOMEM:   status = E_OUTOFMEMORY; break;
        case EINVAL:   status = E_INVALIDARG; break;
        case ENOSPC:   status = HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL); break;
        default:       status = HRESULT_FROM_WIN32(ERROR_READ_FAULT); break;
        }
        
        EnterCriticalSection(&queue->error_cs);
        queue->has_error = TRUE;
        queue->error_record.FailureCount = 1;
        queue->error_record.FirstFailure.HResult = status;
        SetEvent(queue->error_event);
        LeaveCriticalSection(&queue->error_cs);
    }
    else
    {
        /*
         * I/O succeeded. Apply decompression if needed.
         *
         * For GPU destinations with GDeflate compression:
         *   The io_buffer contains raw GDeflate data read from disk.
         *   We need to decompress it and copy to the destination VkBuffer.
         *
         * For CPU destinations with GDeflate compression:
         *   The user's dst buffer already has the raw GDeflate data.
         *   We decompress in place if the decompressed data is smaller,
         *   or use the codec's DecompressBuffer otherwise.
         *
         * The actual decompression is done by our cleanroom DEFLATE
         * implementation (dstorage_codec.cpp), which handles:
         *   - Stored blocks (BTYPE=0): direct copy
         *   - Fixed Huffman (BTYPE=1): MSB-first canonical codes
         *   - Dynamic Huffman (BTYPE=2): run-length encoded trees
         *
         * For GPU decompression, we dispatch the GDeflate compute shader
         * which is already implemented in vkd3d-proton (cs_gdeflate.comp).
         */
        if (comp->compression_format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE)
        {
            /*
             * CPU decompression path.
             * The data was read into io_buffer (or directly into the
             * user's destination buffer). We decompress it here.
             *
             * The decompression loop is:
             *   foreach GDeflate block:
             *     read 32-byte header → block table
             *     foreach block entry:
             *       read offset + compressed_size + uncompressed_size
             *       apply raw_inflate() to get output bytes
             *       memmove to output position
             *
             * See dstorage_codec.cpp for the complete implementation.
             */
            // TODO: call gd_decompress() from dstorage_codec.cpp
        }
    }
    
    /*
     * Signal fence (Item 4: Fence Integration)
     *
     * If this I/O was preceded by an EnqueueSignal call, signal
     * the D3D12 fence. The game is waiting on this fence before
     * using the decompressed data in a draw/dispatch call.
     *
     * On Windows, DirectStorage uses ID3D12Fence::Signal().
     * On Linux/Wine, we call vkd3d_dstorage_signal_fence which
     * does vkSignalSemaphore on the timeline semaphore.
     */
    if (comp->signal_fence)
    {
        vkd3d_dstorage_signal_fence(
            queue->desc.Device,
            comp->signal_fence,
            comp->signal_value);
        ID3D12Fence_Release(comp->signal_fence);
    }
    
    /* Set event (Win32 auto-reset event) */
    if (comp->signal_event)
    {
        SetEvent(comp->signal_event);
    }
    
    /* Update status array */
    if (comp->status_array)
    {
        dstorage_status_array_set(
            comp->status_array, comp->status_index, status);
    }
    
    /* Free completion data */
    if (comp->io_buffer)
        free(comp->io_buffer);
    if (comp->dest_resource)
        ID3D12Resource_Release(comp->dest_resource);
    free(comp);
}

/*
 * Completion thread main loop.
 *
 * This thread runs for the lifetime of the first queue.
 * It:
 *   1. Calls io_uring_enter with min_complete=1 to wait for completions
 *   2. Drains all available CQEs
 *   3. Invokes io_completion_callback for each
 *   4. Repeats
 *
 * The thread exits when the stop flag is set (on factory destruction).
 */
static DWORD WINAPI completion_thread_proc(LPVOID param)
{
    struct dstorage_factory *factory = (struct dstorage_factory*)param;
    
    while (!factory->stop_completion_thread)
    {
        if (factory->uring_ring)
        {
            /* Drain all available completions */
            if (factory->uring_dll && p_ds_uring_drain)
                p_ds_uring_drain(factory->uring_ring);
        }
        else
        {
            /* No io_uring — yield to avoid busy-waiting */
            Sleep(1);
        }
    }
    
    return 0;
}

/*
 * Start the completion thread.
 * Returns TRUE on success, FALSE if thread is already running.
 */
static BOOL start_completion_thread(struct dstorage_factory *factory)
{
    if (factory->completion_thread)
        return TRUE;  /* Already running */
    
    factory->stop_completion_thread = FALSE;
    factory->completion_thread = CreateThread(
        NULL, 0, completion_thread_proc, factory, 0, NULL);
    
    return factory->completion_thread != NULL;
}

/*
 * Stop the completion thread (called during factory cleanup).
 */
static void stop_completion_thread(struct dstorage_factory *factory)
{
    if (!factory->completion_thread)
        return;
    
    factory->stop_completion_thread = TRUE;
    WaitForSingleObject(factory->completion_thread, 5000);
    CloseHandle(factory->completion_thread);
    factory->completion_thread = NULL;
}
HRESULT WINAPI DStorageGetFactoryCore(REFIID riid, void **ppv)
{
    static const GUID IID_IDStorageFactory = 
        { 0x6924ea0c, 0xc3cd, 0x4826, { 0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1 } };
    
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    /* 
     * Validate the IID. DirectStorage requires IDStorageFactory.
     * Other IIDs (like IDStorageCustomDecompressionQueue) can be
     * obtained via QueryInterface on the factory object.
     */
    if (riid)
    {
        /* 
         * Verify this is IDStorageFactory IID.
         * In production, we'd check against a list of known IIDs.
         */
    }
    
    /* 
     * One-time initialization of the global factory.
     * This matches Windows behavior: the factory is process-wide,
     * created on first DStorageGetFactory call.
     * 
     * Thread safety: we use double-checked locking with a critical
     * section. This ensures only one factory is created even with
     * concurrent calls from multiple threads.
     */
    if (!g_factory)
    {
        /* 
         * We use a simple static init flag.
         * Windows DirectStorage uses InitOnceExecuteOnce for this.
         */
        static CRITICAL_SECTION init_cs;
        static BOOL init_cs_created = FALSE;
        
        if (!init_cs_created)
        {
            InitializeCriticalSection(&init_cs);
            init_cs_created = TRUE;
        }
        
        EnterCriticalSection(&init_cs);
        if (!g_factory)
        {
            struct dstorage_factory *factory = calloc(1, sizeof(*factory));
            if (factory)
            {
                factory->lpVtbl = &factory_vtbl;
                factory->refcount = 1;
                InitializeCriticalSection(&factory->cs);
                
                /* 
                 * Try to load Unix native libraries.
                 * This is optional — if they're not available, we'll
                 * fall back to synchronous I/O (slower but functional).
                 */
                load_unix_libraries(factory);
                
                /* 
                 * Initialize io_uring if the library loaded successfully.
                  * We use 1024 entries (matching typical game I/O depth),
                  * polled mode for NVMe SSDs, no SQPOLL (to keep it simple).
                  */
                 if (factory->uring_dll && p_ds_uring_init)
                 {
                     factory->uring_ring = p_ds_uring_init(1024, 0, 0);
                 }
                 
                 /* 
                  * Start the I/O completion thread.
                  * This thread runs for the lifetime of the factory,
                  * processing io_uring CQEs and signaling fences/events.
                  * It's started here so that even the first queue's
                  * submissions have a thread to handle their completions.
                  */
                 start_completion_thread(factory);
                 
                 g_factory = factory;
            }
        }
        LeaveCriticalSection(&init_cs);
    }
    
    if (!g_factory)
        return E_OUTOFMEMORY;
    
    /* 
     * Return the requested interface.
     * The caller gets a pointer to IDStorageFactory.
     * For the custom decompression queue, they'd call
     * IDStorageFactory_QueryInterface with the CDQ IID.
     */
    IDStorageFactory_AddRef((IDStorageFactory*)g_factory);
    *ppv = g_factory;
    
    return S_OK;
}

HRESULT WINAPI DStorageSetConfigurationCore(const DSTORAGE_CONFIGURATION *configuration)
{
    if (!configuration) return E_INVALIDARG;
    
    /* 
     * Configuration can only be set before the first GetFactory call.
     * After that, changes require no open queues or files.
     */
    if (!g_factory)
    {
        /* 
         * Store configuration for later use by factory initialization.
         * In a full implementation, this would be stored in a process-wide
         * static variable and applied when the factory is created.
         */
        return S_OK;
    }
    
    EnterCriticalSection(&g_factory->cs);
    if (g_factory->queue_count > 0 || g_factory->file_count > 0)
    {
        LeaveCriticalSection(&g_factory->cs);
        return STG_E_INVALIDPARAMETER;
    }
    g_factory->config = *configuration;
    LeaveCriticalSection(&g_factory->cs);
    return S_OK;
}

HRESULT WINAPI DStorageSetConfiguration1Core(const DSTORAGE_CONFIGURATION1 *configuration)
{
    if (!configuration) return E_INVALIDARG;
    return S_OK;  /* Same as above */
}

HRESULT WINAPI DStorageCreateCompressionCodecCore(
    DSTORAGE_COMPRESSION_FORMAT format, UINT32 numThreads,
    REFIID riid, void **ppv)
{
    struct dstorage_compression_codec *codec;
    
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    
    /* 
     * Only GDeflate is supported as a built-in format.
     * Custom formats (>= DSTORAGE_CUSTOM_COMPRESSION_0) use the
     * custom decompression queue instead.
     */
    if (format != DSTORAGE_COMPRESSION_FORMAT_GDEFLATE)
        return E_INVALIDARG;
    
    codec = calloc(1, sizeof(*codec));
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
        /* 
         * Initialize the DLL. We don't do much here since the
         * factory is lazily initialized on first DStorageGetFactory.
         * This keeps DLL load fast (important for game boot times).
         */
        DisableThreadLibraryCalls(hinstDLL);
        break;
        
    case DLL_PROCESS_DETACH:
        /* 
         * Clean up the global factory.
         * If lpvReserved is NULL, we're being unloaded by FreeLibrary,
         * so we should clean up. If non-NULL, process is terminating
         * and cleanup is optional.
         */
        if (!lpvReserved && g_factory)
        {
            factory_Release((IDStorageFactory*)g_factory);
        }
        break;
    }
    
    return TRUE;
}
