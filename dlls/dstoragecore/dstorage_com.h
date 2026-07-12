/* dstorage_com.h — DirectStorage COM interface types for Wine PE DLL build
 *
 * This header provides the COM interface definitions that the Microsoft
 * SDK <dstorage.h> normally provides. It is a cleanroom implementation
 * based on public documentation and binary analysis.
 *
 * INCLUDES: dstorage_api.h (for struct/enum/constant definitions)
 * REQUIRES: windows.h (for COM primitives: HRESULT, REFIID, STDMETHODCALLTYPE)
 */
#pragma once
#include <dstorage.h>
#include <assert.h>

/* ------------------------------------------------------------------
 * COM interface type definitions
 *
 * Each interface is defined as a struct with a vtable pointer at offset 0.
 * The vtable struct contains function pointers in the same order as the
 * Microsoft DirectStorage SDK.
 *
 * Inline helper functions follow the Microsoft convention:
 *   Interface_Method(This, args...)  →  This->lpVtbl->Method(This, args...)
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Forward declarations ---- */
typedef struct IDStorageFile                 IDStorageFile;
typedef struct IDStorageQueue                IDStorageQueue;
typedef struct IDStorageQueue1               IDStorageQueue1;
typedef struct IDStorageQueue2               IDStorageQueue2;
typedef struct IDStorageQueue3               IDStorageQueue3;
typedef struct IDStorageStatusArray          IDStorageStatusArray;
typedef struct IDStorageCompressionCodec     IDStorageCompressionCodec;
typedef struct IDStorageCustomDecompressionQueue IDStorageCustomDecompressionQueue;
typedef struct IDStorageFactory              IDStorageFactory;
typedef struct IDStorageFactory1             IDStorageFactory1;

/* ==================================================================
 * IDStorageFile (5 vtable entries)
 * ================================================================== */
typedef struct IDStorageFileVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageFile *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageFile *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageFile *);
    void    (STDMETHODCALLTYPE *Close)(IDStorageFile *);
    HRESULT (STDMETHODCALLTYPE *GetFileInformation)(IDStorageFile *, BY_HANDLE_FILE_INFORMATION *);
} IDStorageFileVtbl;

struct IDStorageFile {
    const struct IDStorageFileVtbl *lpVtbl;
};

FORCEINLINE HRESULT IDStorageFile_QueryInterface(IDStorageFile *This, REFIID riid, void **ppv)
    { return This->lpVtbl->QueryInterface(This, riid, ppv); }
FORCEINLINE ULONG   IDStorageFile_AddRef(IDStorageFile *This)
    { return This->lpVtbl->AddRef(This); }
FORCEINLINE ULONG   IDStorageFile_Release(IDStorageFile *This)
    { return This->lpVtbl->Release(This); }
FORCEINLINE void    IDStorageFile_Close(IDStorageFile *This)
    { This->lpVtbl->Close(This); }
FORCEINLINE HRESULT IDStorageFile_GetFileInformation(IDStorageFile *This, BY_HANDLE_FILE_INFORMATION *info)
    { return This->lpVtbl->GetFileInformation(This, info); }

/* ==================================================================
 * IDStorageStatusArray (5 vtable entries)
 * ================================================================== */
typedef struct IDStorageStatusArrayVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageStatusArray *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageStatusArray *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageStatusArray *);
    BOOL    (STDMETHODCALLTYPE *IsComplete)(IDStorageStatusArray *, UINT32);
    HRESULT (STDMETHODCALLTYPE *GetHResult)(IDStorageStatusArray *, UINT32);
} IDStorageStatusArrayVtbl;

struct IDStorageStatusArray {
    const struct IDStorageStatusArrayVtbl *lpVtbl;
};

FORCEINLINE HRESULT IDStorageStatusArray_QueryInterface(IDStorageStatusArray *This, REFIID riid, void **ppv)
    { return This->lpVtbl->QueryInterface(This, riid, ppv); }
FORCEINLINE ULONG   IDStorageStatusArray_AddRef(IDStorageStatusArray *This)
    { return This->lpVtbl->AddRef(This); }
FORCEINLINE ULONG   IDStorageStatusArray_Release(IDStorageStatusArray *This)
    { return This->lpVtbl->Release(This); }
FORCEINLINE BOOL    IDStorageStatusArray_IsComplete(IDStorageStatusArray *This, UINT32 index)
    { return This->lpVtbl->IsComplete(This, index); }
FORCEINLINE HRESULT IDStorageStatusArray_GetHResult(IDStorageStatusArray *This, UINT32 index)
    { return This->lpVtbl->GetHResult(This, index); }

/* ==================================================================
 * IDStorageCompressionCodec (6 vtable entries)
 * ================================================================== */
typedef struct IDStorageCompressionCodecVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageCompressionCodec *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageCompressionCodec *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageCompressionCodec *);
    HRESULT (STDMETHODCALLTYPE *CompressBuffer)(IDStorageCompressionCodec *,
              const void *, size_t, DSTORAGE_COMPRESSION,
              void *, size_t, size_t *);
    HRESULT (STDMETHODCALLTYPE *DecompressBuffer)(IDStorageCompressionCodec *,
              const void *, size_t, void *, size_t, size_t *);
    size_t  (STDMETHODCALLTYPE *CompressBufferBound)(IDStorageCompressionCodec *, size_t);
} IDStorageCompressionCodecVtbl;

struct IDStorageCompressionCodec {
    const struct IDStorageCompressionCodecVtbl *lpVtbl;
};

FORCEINLINE HRESULT IDStorageCompressionCodec_QueryInterface(IDStorageCompressionCodec *This, REFIID riid, void **ppv)
    { return This->lpVtbl->QueryInterface(This, riid, ppv); }
FORCEINLINE ULONG   IDStorageCompressionCodec_AddRef(IDStorageCompressionCodec *This)
    { return This->lpVtbl->AddRef(This); }
FORCEINLINE ULONG   IDStorageCompressionCodec_Release(IDStorageCompressionCodec *This)
    { return This->lpVtbl->Release(This); }
FORCEINLINE HRESULT IDStorageCompressionCodec_CompressBuffer(IDStorageCompressionCodec *This,
    const void *uncompressed, size_t uncompressedSize, DSTORAGE_COMPRESSION compression,
    void *compressed, size_t compressedSize, size_t *compressedDataSize)
    { return This->lpVtbl->CompressBuffer(This, uncompressed, uncompressedSize, compression, compressed, compressedSize, compressedDataSize); }
FORCEINLINE HRESULT IDStorageCompressionCodec_DecompressBuffer(IDStorageCompressionCodec *This,
    const void *compressed, size_t compressedSize, void *uncompressed, size_t uncompressedSize,
    size_t *uncompressedDataSize)
    { return This->lpVtbl->DecompressBuffer(This, compressed, compressedSize, uncompressed, uncompressedSize, uncompressedDataSize); }
FORCEINLINE size_t  IDStorageCompressionCodec_CompressBufferBound(IDStorageCompressionCodec *This, size_t uncompressedSize)
    { return This->lpVtbl->CompressBufferBound(This, uncompressedSize); }

/* ==================================================================
 * IDStorageQueue base interface
 * (12 vtable entries: 3 IUnknown + 9 queue-specific)
 * ================================================================== */
typedef struct IDStorageQueueVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageQueue *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageQueue *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *EnqueueRequest)(IDStorageQueue *, const DSTORAGE_REQUEST *);
    void    (STDMETHODCALLTYPE *EnqueueStatus)(IDStorageQueue *, IDStorageStatusArray *, UINT32);
    void    (STDMETHODCALLTYPE *EnqueueSignal)(IDStorageQueue *, ID3D12Fence *, UINT64);
    void    (STDMETHODCALLTYPE *Submit)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *CancelRequestsWithTag)(IDStorageQueue *, UINT64, UINT64);
    void    (STDMETHODCALLTYPE *Close)(IDStorageQueue *);
    HANDLE  (STDMETHODCALLTYPE *GetErrorEvent)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *RetrieveErrorRecord)(IDStorageQueue *, DSTORAGE_ERROR_RECORD *);
    void    (STDMETHODCALLTYPE *Query)(IDStorageQueue *, DSTORAGE_QUEUE_INFO *);
} IDStorageQueueVtbl;

struct IDStorageQueue {
    const struct IDStorageQueueVtbl *lpVtbl;
};

FORCEINLINE HRESULT IDStorageQueue_QueryInterface(IDStorageQueue *This, REFIID riid, void **ppv)
    { return This->lpVtbl->QueryInterface(This, riid, ppv); }
FORCEINLINE ULONG   IDStorageQueue_AddRef(IDStorageQueue *This)
    { return This->lpVtbl->AddRef(This); }
FORCEINLINE ULONG   IDStorageQueue_Release(IDStorageQueue *This)
    { return This->lpVtbl->Release(This); }
FORCEINLINE void    IDStorageQueue_EnqueueRequest(IDStorageQueue *This, const DSTORAGE_REQUEST *req)
    { This->lpVtbl->EnqueueRequest(This, req); }
FORCEINLINE void    IDStorageQueue_EnqueueStatus(IDStorageQueue *This, IDStorageStatusArray *arr, UINT32 idx)
    { This->lpVtbl->EnqueueStatus(This, arr, idx); }
FORCEINLINE void    IDStorageQueue_EnqueueSignal(IDStorageQueue *This, ID3D12Fence *fence, UINT64 val)
    { This->lpVtbl->EnqueueSignal(This, fence, val); }
FORCEINLINE void    IDStorageQueue_Submit(IDStorageQueue *This)
    { This->lpVtbl->Submit(This); }
FORCEINLINE void    IDStorageQueue_CancelRequestsWithTag(IDStorageQueue *This, UINT64 mask, UINT64 value)
    { This->lpVtbl->CancelRequestsWithTag(This, mask, value); }
FORCEINLINE void    IDStorageQueue_Close(IDStorageQueue *This)
    { This->lpVtbl->Close(This); }
FORCEINLINE HANDLE  IDStorageQueue_GetErrorEvent(IDStorageQueue *This)
    { return This->lpVtbl->GetErrorEvent(This); }
FORCEINLINE void    IDStorageQueue_RetrieveErrorRecord(IDStorageQueue *This, DSTORAGE_ERROR_RECORD *rec)
    { This->lpVtbl->RetrieveErrorRecord(This, rec); }
FORCEINLINE void    IDStorageQueue_Query(IDStorageQueue *This, DSTORAGE_QUEUE_INFO *info)
    { This->lpVtbl->Query(This, info); }

/* ------------------------------------------------------------------
 * IDStorageQueue1 extends IDStorageQueue (+1: EnqueueSetEvent)
 * ------------------------------------------------------------------ */
typedef struct IDStorageQueue1Vtbl {
    /* Inherited IDStorageQueue (0-11) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageQueue *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageQueue *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *EnqueueRequest)(IDStorageQueue *, const DSTORAGE_REQUEST *);
    void    (STDMETHODCALLTYPE *EnqueueStatus)(IDStorageQueue *, IDStorageStatusArray *, UINT32);
    void    (STDMETHODCALLTYPE *EnqueueSignal)(IDStorageQueue *, ID3D12Fence *, UINT64);
    void    (STDMETHODCALLTYPE *Submit)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *CancelRequestsWithTag)(IDStorageQueue *, UINT64, UINT64);
    void    (STDMETHODCALLTYPE *Close)(IDStorageQueue *);
    HANDLE  (STDMETHODCALLTYPE *GetErrorEvent)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *RetrieveErrorRecord)(IDStorageQueue *, DSTORAGE_ERROR_RECORD *);
    void    (STDMETHODCALLTYPE *Query)(IDStorageQueue *, DSTORAGE_QUEUE_INFO *);
    /* IDStorageQueue1 */
    void    (STDMETHODCALLTYPE *EnqueueSetEvent)(IDStorageQueue *, HANDLE);
} IDStorageQueue1Vtbl;

struct IDStorageQueue1 {
    const struct IDStorageQueue1Vtbl *lpVtbl;
};

/* ------------------------------------------------------------------
 * IDStorageQueue2 extends IDStorageQueue1 (+1: GetCompressionSupport)
 * ------------------------------------------------------------------ */
typedef struct IDStorageQueue2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageQueue *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageQueue *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *EnqueueRequest)(IDStorageQueue *, const DSTORAGE_REQUEST *);
    void    (STDMETHODCALLTYPE *EnqueueStatus)(IDStorageQueue *, IDStorageStatusArray *, UINT32);
    void    (STDMETHODCALLTYPE *EnqueueSignal)(IDStorageQueue *, ID3D12Fence *, UINT64);
    void    (STDMETHODCALLTYPE *Submit)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *CancelRequestsWithTag)(IDStorageQueue *, UINT64, UINT64);
    void    (STDMETHODCALLTYPE *Close)(IDStorageQueue *);
    HANDLE  (STDMETHODCALLTYPE *GetErrorEvent)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *RetrieveErrorRecord)(IDStorageQueue *, DSTORAGE_ERROR_RECORD *);
    void    (STDMETHODCALLTYPE *Query)(IDStorageQueue *, DSTORAGE_QUEUE_INFO *);
    void    (STDMETHODCALLTYPE *EnqueueSetEvent)(IDStorageQueue *, HANDLE);
    DSTORAGE_COMPRESSION_SUPPORT (STDMETHODCALLTYPE *GetCompressionSupport)(IDStorageQueue *, DSTORAGE_COMPRESSION_FORMAT);
} IDStorageQueue2Vtbl;

struct IDStorageQueue2 {
    const struct IDStorageQueue2Vtbl *lpVtbl;
};

/* ------------------------------------------------------------------
 * IDStorageQueue3 extends IDStorageQueue2 (+1: EnqueueRequests)
 * ------------------------------------------------------------------ */
typedef struct IDStorageQueue3Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageQueue *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageQueue *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *EnqueueRequest)(IDStorageQueue *, const DSTORAGE_REQUEST *);
    void    (STDMETHODCALLTYPE *EnqueueStatus)(IDStorageQueue *, IDStorageStatusArray *, UINT32);
    void    (STDMETHODCALLTYPE *EnqueueSignal)(IDStorageQueue *, ID3D12Fence *, UINT64);
    void    (STDMETHODCALLTYPE *Submit)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *CancelRequestsWithTag)(IDStorageQueue *, UINT64, UINT64);
    void    (STDMETHODCALLTYPE *Close)(IDStorageQueue *);
    HANDLE  (STDMETHODCALLTYPE *GetErrorEvent)(IDStorageQueue *);
    void    (STDMETHODCALLTYPE *RetrieveErrorRecord)(IDStorageQueue *, DSTORAGE_ERROR_RECORD *);
    void    (STDMETHODCALLTYPE *Query)(IDStorageQueue *, DSTORAGE_QUEUE_INFO *);
    void    (STDMETHODCALLTYPE *EnqueueSetEvent)(IDStorageQueue *, HANDLE);
    DSTORAGE_COMPRESSION_SUPPORT (STDMETHODCALLTYPE *GetCompressionSupport)(IDStorageQueue *, DSTORAGE_COMPRESSION_FORMAT);
    void    (STDMETHODCALLTYPE *EnqueueRequests)(IDStorageQueue *, UINT32, const DSTORAGE_REQUEST *);
} IDStorageQueue3Vtbl;

struct IDStorageQueue3 {
    const struct IDStorageQueue3Vtbl *lpVtbl;
};

/* ==================================================================
 * IDStorageCustomDecompressionQueue (4 vtable entries)
 * ================================================================== */
typedef struct IDStorageCustomDecompressionQueueVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageCustomDecompressionQueue *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageCustomDecompressionQueue *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageCustomDecompressionQueue *);
    void    (STDMETHODCALLTYPE *GetEvent)(IDStorageCustomDecompressionQueue *, HANDLE *);
    HRESULT (STDMETHODCALLTYPE *GetRequests)(IDStorageCustomDecompressionQueue *, UINT32 *, UINT32, DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST *);
    HRESULT (STDMETHODCALLTYPE *SetRequestResults)(IDStorageCustomDecompressionQueue *, UINT32, DSTORAGE_CUSTOM_DECOMPRESSION_RESULT *);
} IDStorageCustomDecompressionQueueVtbl;

struct IDStorageCustomDecompressionQueue {
    const struct IDStorageCustomDecompressionQueueVtbl *lpVtbl;
};

/* ==================================================================
 * IDStorageFactory (8 vtable entries)
 *
 * NOTE: struct dstorage_factory in dstoragecore_main.c MUST have
 * the same first member layout (lpVtbl at offset 0) for COM casting.
 * ================================================================== */
typedef struct IDStorageFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageFactory *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageFactory *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageFactory *);
    HRESULT (STDMETHODCALLTYPE *CreateQueue)(IDStorageFactory *, const DSTORAGE_QUEUE_DESC *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *OpenFile)(IDStorageFactory *, const WCHAR *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *CreateStatusArray)(IDStorageFactory *, UINT32, PCSTR, REFIID, void **);
    void    (STDMETHODCALLTYPE *SetDebugFlags)(IDStorageFactory *, UINT32);
    HRESULT (STDMETHODCALLTYPE *SetStagingBufferSize)(IDStorageFactory *, UINT32);
} IDStorageFactoryVtbl;

struct IDStorageFactory {
    const struct IDStorageFactoryVtbl *lpVtbl;
};

FORCEINLINE HRESULT IDStorageFactory_QueryInterface(IDStorageFactory *This, REFIID riid, void **ppv)
    { return This->lpVtbl->QueryInterface(This, riid, ppv); }
FORCEINLINE ULONG   IDStorageFactory_AddRef(IDStorageFactory *This)
    { return This->lpVtbl->AddRef(This); }
FORCEINLINE ULONG   IDStorageFactory_Release(IDStorageFactory *This)
    { return This->lpVtbl->Release(This); }
FORCEINLINE HRESULT IDStorageFactory_CreateQueue(IDStorageFactory *This, const DSTORAGE_QUEUE_DESC *desc, REFIID riid, void **ppv)
    { return This->lpVtbl->CreateQueue(This, desc, riid, ppv); }
FORCEINLINE HRESULT IDStorageFactory_OpenFile(IDStorageFactory *This, const WCHAR *path, REFIID riid, void **ppv)
    { return This->lpVtbl->OpenFile(This, path, riid, ppv); }
FORCEINLINE HRESULT IDStorageFactory_CreateStatusArray(IDStorageFactory *This, UINT32 capacity, PCSTR name, REFIID riid, void **ppv)
    { return This->lpVtbl->CreateStatusArray(This, capacity, name, riid, ppv); }
FORCEINLINE void    IDStorageFactory_SetDebugFlags(IDStorageFactory *This, UINT32 flags)
    { This->lpVtbl->SetDebugFlags(This, flags); }
FORCEINLINE HRESULT IDStorageFactory_SetStagingBufferSize(IDStorageFactory *This, UINT32 size)
    { return This->lpVtbl->SetStagingBufferSize(This, size); }

/* ==================================================================
 * IDStorageFactory1 extends IDStorageFactory (+1: GetCompressionCodec)
 * ================================================================== */
typedef struct IDStorageFactory1Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDStorageFactory *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IDStorageFactory *);
    ULONG   (STDMETHODCALLTYPE *Release)(IDStorageFactory *);
    HRESULT (STDMETHODCALLTYPE *CreateQueue)(IDStorageFactory *, const DSTORAGE_QUEUE_DESC *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *OpenFile)(IDStorageFactory *, const WCHAR *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *CreateStatusArray)(IDStorageFactory *, UINT32, PCSTR, REFIID, void **);
    void    (STDMETHODCALLTYPE *SetDebugFlags)(IDStorageFactory *, UINT32);
    HRESULT (STDMETHODCALLTYPE *SetStagingBufferSize)(IDStorageFactory *, UINT32);
    HRESULT (STDMETHODCALLTYPE *GetCompressionCodec)(IDStorageFactory *, DSTORAGE_COMPRESSION_FORMAT, REFIID, void **);
} IDStorageFactory1Vtbl;

struct IDStorageFactory1 {
    const struct IDStorageFactory1Vtbl *lpVtbl;
};

/* ==================================================================
 * IID GUIDs
 *
 * These are the standard DirectStorage interface identifiers from the
 * Microsoft DirectStorage SDK.
 *
 * NOTE: IID_IDStorageFile and IID_IDStorageCompressionCodec are
 * defined locally in the respective QueryInterface functions in
 * dstoragecore_main.c, matching the pattern used for all other IIDs.
 * ================================================================== */

/* ==================================================================
 * Error code aliases
 *
 * The Microsoft DirectStorage SDK uses DSTORAGE_E_* prefixes.
 * Our cleanroom header uses DS_E_* internally. Provide aliases
 * for the error codes used by dstoragecore_main.c.
 * ================================================================== */
#define DSTORAGE_E_INVALID_MEMORY_QUEUE_PRIORITY DS_E_INVALID_MEMORY_QUEUE_PRIORITY

/* ==================================================================
 * D3D12 COM helper macros
 *
 * MinGW's d3d12.h does not define the inline COM helper functions
 * that Microsoft's headers provide. Define the ones we need.
 * ================================================================== */

/* ID3D12Fence inherits from ID3D12Pageable → ID3D12DeviceChild → ID3D12Object → IUnknown */
/* AddRef/Release are at fixed vtable offsets (1 and 2 after QueryInterface) for all COM. */
#define ID3D12Fence_AddRef(This)        ((This)->lpVtbl->AddRef((This)))
#define ID3D12Fence_Release(This)       ((This)->lpVtbl->Release((This)))
#define ID3D12Resource_Release(This)    ((This)->lpVtbl->Release((This)))

#ifdef __cplusplus
}
#endif
