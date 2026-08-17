// SPDX-License-Identifier: Apache-2.0
// Cleanroom implementation of Microsoft DirectStorage-style API for Linux.
// This is a cleanroom implementation based on public documentation and
// binary analysis - no Microsoft code is included or derived.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version / SDK identification
// ---------------------------------------------------------------------------
#define DSTORAGE_SDK_VERSION 300

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define DSTORAGE_MIN_QUEUE_CAPACITY 0x80
#define DSTORAGE_MAX_QUEUE_CAPACITY 0x2000
#define DSTORAGE_REQUEST_MAX_NAME 64
#define DSTORAGE_DISABLE_BUILTIN_CPU_DECOMPRESSION (-1)

// ---------------------------------------------------------------------------
// HRESULT-style error codes (facility 0x892 = GAME facility 2340)
// All are int32_t for ABI compatibility with Windows HRESULT.
// ---------------------------------------------------------------------------
#define DS_STATUS_OK                    ((int32_t)0)
#define DS_STATUS_PENDING               ((int32_t)0x89240000)
#define DS_E_ACCESS_VIOLATION           ((int32_t)0x89240009)
#define DS_E_ALREADY_RUNNING            ((int32_t)0x89240001)
#define DS_E_COMPRESSED_DATA_TOO_LARGE  ((int32_t)0x89240039)
#define DS_E_DECOMPRESSION_ERROR        ((int32_t)0x89240030)
#define DS_E_END_OF_FILE                ((int32_t)0x89240007)
#define DS_E_FILE_NOT_OPEN              ((int32_t)0x8924000B)
#define DS_E_INDEX_BOUND                ((int32_t)0x89240015)
#define DS_E_INVALID_DESTINATION_SIZE   ((int32_t)0x8924000F)
#define DS_E_INVALID_FENCE              ((int32_t)0x89240022)
#define DS_E_INVALID_FILE_HANDLE        ((int32_t)0x89240017)
#define DS_E_INVALID_FILE_OFFSET        ((int32_t)0x8924001A)
#define DS_E_INVALID_MEMORY_QUEUE_PRIORITY ((int32_t)0x89240024)
#define DS_E_INVALID_QUEUE_CAPACITY     ((int32_t)0x89240003)
#define DS_E_INVALID_QUEUE_PRIORITY     ((int32_t)0x89240013)
#define DS_E_INVALID_SOURCE_TYPE        ((int32_t)0x8924001B)
#define DS_E_INVALID_STAGING_BUFFER_SIZE ((int32_t)0x89240020)
#define DS_E_INVALID_STATUS_ARRAY       ((int32_t)0x89240023)
#define DS_E_IO_TIMEOUT                 ((int32_t)0x89240016)
#define DS_E_NOT_RUNNING                ((int32_t)0x89240002)
#define DS_E_QUEUE_CLOSED               ((int32_t)0x89240010)
#define DS_E_REQUEST_TOO_LARGE          ((int32_t)0x89240008)
#define DS_E_RESERVED_FIELDS            ((int32_t)0x8924000C)
#define DS_E_STAGING_BUFFER_LOCKED      ((int32_t)0x8924001F)
#define DS_E_STAGING_BUFFER_TOO_SMALL   ((int32_t)0x89240021)
#define DS_E_TOO_MANY_FILES             ((int32_t)0x89240014)
#define DS_E_TOO_MANY_QUEUES            ((int32_t)0x89240012)
#define DS_E_INVALID_DESTINATION_TYPE   ((int32_t)0x89240040)
#define DS_E_FILEBUFFERING_REQUIRES_DISABLED_BYPASSIO ((int32_t)0x89240041)
#define DS_E_INVALID_CLUSTER_SIZE       ((int32_t)0x89240011)

// ---------------------------------------------------------------------------
// Opaque handle types
// ---------------------------------------------------------------------------
typedef struct DStorageFactory DStorageFactory;
typedef struct DStorageQueue DStorageQueue;
typedef struct DStorageFile DStorageFile;
typedef struct DStorageStatusArray DStorageStatusArray;
typedef struct DStorageCompressionCodec DStorageCompressionCodec;
typedef struct DStorageCustomDecompressionQueue DStorageCustomDecompressionQueue;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
typedef int8_t DSTORAGE_PRIORITY;
#define DSTORAGE_PRIORITY_LOW     ((DSTORAGE_PRIORITY)(-1))
#define DSTORAGE_PRIORITY_NORMAL  ((DSTORAGE_PRIORITY)0)
#define DSTORAGE_PRIORITY_HIGH    ((DSTORAGE_PRIORITY)1)
#define DSTORAGE_PRIORITY_REALTIME ((DSTORAGE_PRIORITY)2)
#define DSTORAGE_PRIORITY_FIRST   DSTORAGE_PRIORITY_LOW
#define DSTORAGE_PRIORITY_LAST    DSTORAGE_PRIORITY_REALTIME
#define DSTORAGE_PRIORITY_COUNT   4

typedef uint8_t DSTORAGE_COMPRESSION_FORMAT;
#define DSTORAGE_COMPRESSION_FORMAT_NONE     ((DSTORAGE_COMPRESSION_FORMAT)0)
#define DSTORAGE_COMPRESSION_FORMAT_GDEFLATE ((DSTORAGE_COMPRESSION_FORMAT)1)
#define DSTORAGE_CUSTOM_COMPRESSION_0        ((DSTORAGE_COMPRESSION_FORMAT)0x80)

typedef uint64_t DSTORAGE_REQUEST_SOURCE_TYPE;
#define DSTORAGE_REQUEST_SOURCE_FILE   ((DSTORAGE_REQUEST_SOURCE_TYPE)0)
#define DSTORAGE_REQUEST_SOURCE_MEMORY ((DSTORAGE_REQUEST_SOURCE_TYPE)1)

typedef uint64_t DSTORAGE_REQUEST_DESTINATION_TYPE;
#define DSTORAGE_REQUEST_DESTINATION_MEMORY                  ((DSTORAGE_REQUEST_DESTINATION_TYPE)0)
#define DSTORAGE_REQUEST_DESTINATION_BUFFER                  ((DSTORAGE_REQUEST_DESTINATION_TYPE)1)
#define DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION          ((DSTORAGE_REQUEST_DESTINATION_TYPE)2)
#define DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES   ((DSTORAGE_REQUEST_DESTINATION_TYPE)3)
#define DSTORAGE_REQUEST_DESTINATION_TILES                   ((DSTORAGE_REQUEST_DESTINATION_TYPE)4)
#define DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE ((DSTORAGE_REQUEST_DESTINATION_TYPE)5)

typedef int8_t DSTORAGE_COMMAND_TYPE;
#define DSTORAGE_COMMAND_TYPE_NONE    ((DSTORAGE_COMMAND_TYPE)(-1))
#define DSTORAGE_COMMAND_TYPE_REQUEST ((DSTORAGE_COMMAND_TYPE)0)
#define DSTORAGE_COMMAND_TYPE_STATUS  ((DSTORAGE_COMMAND_TYPE)1)
#define DSTORAGE_COMMAND_TYPE_SIGNAL  ((DSTORAGE_COMMAND_TYPE)2)
#define DSTORAGE_COMMAND_TYPE_EVENT   ((DSTORAGE_COMMAND_TYPE)3)

typedef int32_t DSTORAGE_COMPRESSION;
#define DSTORAGE_COMPRESSION_FASTEST    ((DSTORAGE_COMPRESSION)(-1))
#define DSTORAGE_COMPRESSION_DEFAULT    ((DSTORAGE_COMPRESSION)0)
#define DSTORAGE_COMPRESSION_BEST_RATIO ((DSTORAGE_COMPRESSION)1)

typedef uint32_t DSTORAGE_STAGING_BUFFER_SIZE;
#define DSTORAGE_STAGING_BUFFER_SIZE_0    ((DSTORAGE_STAGING_BUFFER_SIZE)0)
#define DSTORAGE_STAGING_BUFFER_SIZE_32MB ((DSTORAGE_STAGING_BUFFER_SIZE)(32 * 1048576))

typedef uint32_t DSTORAGE_DEBUG_FLAGS;
#define DSTORAGE_DEBUG_NONE               ((DSTORAGE_DEBUG_FLAGS)0x00)
#define DSTORAGE_DEBUG_SHOW_ERRORS        ((DSTORAGE_DEBUG_FLAGS)0x01)
#define DSTORAGE_DEBUG_BREAK_ON_ERROR     ((DSTORAGE_DEBUG_FLAGS)0x02)
#define DSTORAGE_DEBUG_RECORD_OBJECT_NAMES ((DSTORAGE_DEBUG_FLAGS)0x04)

typedef uint32_t DSTORAGE_GET_REQUEST_FLAGS;
#define DSTORAGE_GET_REQUEST_FLAG_SELECT_CUSTOM  ((DSTORAGE_GET_REQUEST_FLAGS)0x01)
#define DSTORAGE_GET_REQUEST_FLAG_SELECT_BUILTIN ((DSTORAGE_GET_REQUEST_FLAGS)0x02)
#define DSTORAGE_GET_REQUEST_FLAG_SELECT_ALL     ((DSTORAGE_GET_REQUEST_FLAGS)0x03)

typedef uint32_t DSTORAGE_CUSTOM_DECOMPRESSION_FLAGS;
#define DSTORAGE_CUSTOM_DECOMPRESSION_FLAG_NONE            ((DSTORAGE_CUSTOM_DECOMPRESSION_FLAGS)0x00)
#define DSTORAGE_CUSTOM_DECOMPRESSION_FLAG_DEST_IN_UPLOAD_HEAP ((DSTORAGE_CUSTOM_DECOMPRESSION_FLAGS)0x01)

typedef uint32_t DSTORAGE_COMPRESSION_SUPPORT;
#define DSTORAGE_COMPRESSION_SUPPORT_NONE             ((DSTORAGE_COMPRESSION_SUPPORT)0x00)
#define DSTORAGE_COMPRESSION_SUPPORT_GPU_OPTIMIZED    ((DSTORAGE_COMPRESSION_SUPPORT)0x01)
#define DSTORAGE_COMPRESSION_SUPPORT_GPU_FALLBACK     ((DSTORAGE_COMPRESSION_SUPPORT)0x02)
#define DSTORAGE_COMPRESSION_SUPPORT_CPU_FALLBACK     ((DSTORAGE_COMPRESSION_SUPPORT)0x04)
#define DSTORAGE_COMPRESSION_SUPPORT_USES_COMPUTE_QUEUE ((DSTORAGE_COMPRESSION_SUPPORT)0x08)
#define DSTORAGE_COMPRESSION_SUPPORT_USES_COPY_QUEUE  ((DSTORAGE_COMPRESSION_SUPPORT)0x10)

typedef uint32_t DSTORAGE_ENQUEUE_REQUEST_FLAGS;
#define DSTORAGE_ENQUEUE_REQUEST_FLAG_NONE                      ((DSTORAGE_ENQUEUE_REQUEST_FLAGS)0)
#define DSTORAGE_ENQUEUE_REQUEST_FLAG_FENCE_WAIT_BEFORE_GPU_WORK ((DSTORAGE_ENQUEUE_REQUEST_FLAGS)1)
#define DSTORAGE_ENQUEUE_REQUEST_FLAG_FENCE_WAIT_BEFORE_SOURCE_ACCESS ((DSTORAGE_ENQUEUE_REQUEST_FLAGS)2)

// ---------------------------------------------------------------------------
// Structures — 8-byte packing to match MSVC /Zp8 (Windows x64 default)
// ---------------------------------------------------------------------------
#pragma pack(push, 8)

typedef struct DSTORAGE_QUEUE_DESC {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    uint16_t                     Capacity;
    DSTORAGE_PRIORITY            Priority;
    const char*                  Name;
    void*                        Device;
} DSTORAGE_QUEUE_DESC;

typedef struct DSTORAGE_QUEUE_INFO {
    DSTORAGE_QUEUE_DESC Desc;
    uint16_t            EmptySlotCount;
    uint16_t            RequestCountUntilAutoSubmit;
} DSTORAGE_QUEUE_INFO;

typedef struct DSTORAGE_SOURCE_FILE {
    struct DStorageFile* Source;
    uint64_t             Offset;
    uint32_t             Size;
} DSTORAGE_SOURCE_FILE;

typedef struct DSTORAGE_SOURCE_MEMORY {
    const void* Source;
    uint32_t    Size;
} DSTORAGE_SOURCE_MEMORY;

typedef union DSTORAGE_SOURCE {
    DSTORAGE_SOURCE_MEMORY Memory;
    DSTORAGE_SOURCE_FILE File;
} DSTORAGE_SOURCE;

typedef struct DSTORAGE_DESTINATION_MEMORY {
    void*   Buffer;
    uint32_t Size;
} DSTORAGE_DESTINATION_MEMORY;

typedef struct DSTORAGE_DESTINATION_BUFFER {
    void*   Resource;
    uint64_t Offset;
    uint32_t Size;
} DSTORAGE_DESTINATION_BUFFER;

typedef struct DSTORAGE_DESTINATION_TEXTURE_REGION {
    void*    Resource;
    uint32_t SubresourceIndex;
    uint32_t Region[6];
} DSTORAGE_DESTINATION_TEXTURE_REGION;

typedef struct DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES {
    void* Resource;
    uint32_t FirstSubresource;
} DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES;

typedef struct DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE {
    void*    Resource;
    uint32_t FirstSubresource;
    uint32_t NumSubresources;
} DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE;

typedef struct DSTORAGE_DESTINATION_TILES {
    void*    Resource;
    uint32_t TiledRegionStartCoordinate[3];
    uint32_t TileRegionSize[3];
} DSTORAGE_DESTINATION_TILES;

typedef union DSTORAGE_DESTINATION {
    DSTORAGE_DESTINATION_MEMORY Memory;
    DSTORAGE_DESTINATION_BUFFER Buffer;
    DSTORAGE_DESTINATION_TEXTURE_REGION Texture;
    DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES MultipleSubresources;
    DSTORAGE_DESTINATION_TILES Tiles;
    DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE MultipleSubresourcesRange;
} DSTORAGE_DESTINATION;

typedef struct DSTORAGE_REQUEST_OPTIONS {
    uint8_t  CompressionFormat;  // DSTORAGE_COMPRESSION_FORMAT : 8
    uint8_t  Reserved1[7];       // padding to 8 bytes
    union {
        uint64_t SourceAndDest;  // Combined bitfield: SourceType:1 + DestinationType:7 + Reserved:48
        struct {
            uint64_t SourceType : 1;      // DSTORAGE_REQUEST_SOURCE_TYPE
            uint64_t DestinationType : 7; // DSTORAGE_REQUEST_DESTINATION_TYPE
            uint64_t Reserved2 : 56;      // Reserved
        };
    };
} DSTORAGE_REQUEST_OPTIONS;

static inline DSTORAGE_REQUEST_SOURCE_TYPE DSTORAGE_GET_SOURCE_TYPE(const DSTORAGE_REQUEST_OPTIONS* o) {
    return (DSTORAGE_REQUEST_SOURCE_TYPE)(o->SourceAndDest & 1);
}
static inline DSTORAGE_REQUEST_DESTINATION_TYPE DSTORAGE_GET_DEST_TYPE(const DSTORAGE_REQUEST_OPTIONS* o) {
    return (DSTORAGE_REQUEST_DESTINATION_TYPE)((o->SourceAndDest >> 1) & 0x7F);
}
static inline void DSTORAGE_SET_SOURCE_TYPE(DSTORAGE_REQUEST_OPTIONS* o, DSTORAGE_REQUEST_SOURCE_TYPE v) {
    o->SourceAndDest = (o->SourceAndDest & ~1ULL) | ((uint64_t)v & 1);
}
static inline void DSTORAGE_SET_DEST_TYPE(DSTORAGE_REQUEST_OPTIONS* o, DSTORAGE_REQUEST_DESTINATION_TYPE v) {
    o->SourceAndDest = (o->SourceAndDest & ~(0x7FULL << 1)) | (((uint64_t)v & 0x7F) << 1);
}

typedef struct DSTORAGE_REQUEST {
    DSTORAGE_REQUEST_OPTIONS Options;
    DSTORAGE_SOURCE          Source;
    DSTORAGE_DESTINATION     Destination;
    uint32_t                 UncompressedSize;
    uint64_t                 CancellationTag;
    const char*              Name;
} DSTORAGE_REQUEST;

typedef struct DSTORAGE_ERROR_PARAMETERS_REQUEST {
    uint16_t        Filename[260];
    char            RequestName[64];
    DSTORAGE_REQUEST Request;
} DSTORAGE_ERROR_PARAMETERS_REQUEST;

typedef struct DSTORAGE_ERROR_PARAMETERS_STATUS {
    struct DStorageStatusArray* StatusArray;
    uint32_t                    Index;
} DSTORAGE_ERROR_PARAMETERS_STATUS;

typedef struct DSTORAGE_ERROR_PARAMETERS_SIGNAL {
    void*    Fence;
    uint64_t Value;
} DSTORAGE_ERROR_PARAMETERS_SIGNAL;

typedef struct DSTORAGE_ERROR_PARAMETERS_EVENT {
    void* Handle;
} DSTORAGE_ERROR_PARAMETERS_EVENT;

typedef struct DSTORAGE_ERROR_FIRST_FAILURE {
    int32_t      HResult;
    DSTORAGE_COMMAND_TYPE CommandType;
    union {
        DSTORAGE_ERROR_PARAMETERS_REQUEST Request;
        DSTORAGE_ERROR_PARAMETERS_STATUS Status;
        DSTORAGE_ERROR_PARAMETERS_SIGNAL Signal;
        DSTORAGE_ERROR_PARAMETERS_EVENT Event;
    };
} DSTORAGE_ERROR_FIRST_FAILURE;

typedef struct DSTORAGE_ERROR_RECORD {
    uint32_t                    FailureCount;
    DSTORAGE_ERROR_FIRST_FAILURE FirstFailure;
} DSTORAGE_ERROR_RECORD;

typedef struct DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST {
    uint64_t                          Id;
    DSTORAGE_COMPRESSION_FORMAT       CompressionFormat;
    uint8_t                           Reserved[3];
    DSTORAGE_CUSTOM_DECOMPRESSION_FLAGS Flags;
    uint64_t                          SrcSize;
    const void*                       SrcBuffer;
    uint64_t                          DstSize;
    void*                             DstBuffer;
} DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST;

typedef struct DSTORAGE_CUSTOM_DECOMPRESSION_RESULT {
    uint64_t Id;
    int32_t  Result;
} DSTORAGE_CUSTOM_DECOMPRESSION_RESULT;

typedef struct DSTORAGE_CONFIGURATION {
    uint32_t NumSubmitThreads;
    int32_t  NumBuiltInCpuDecompressionThreads;
    int32_t  ForceMappingLayer;
    int32_t  DisableBypassIO;
    int32_t  DisableTelemetry;
    int32_t  DisableGpuDecompressionMetacommand;
    int32_t  DisableGpuDecompression;
} DSTORAGE_CONFIGURATION;

typedef struct DSTORAGE_CONFIGURATION1 {
    uint32_t NumSubmitThreads;
    int32_t  NumBuiltInCpuDecompressionThreads;
    int32_t  ForceMappingLayer;
    int32_t  DisableBypassIO;
    int32_t  DisableTelemetry;
    int32_t  DisableGpuDecompressionMetacommand;
    int32_t  DisableGpuDecompression;
    int32_t  ForceFileBuffering;
} DSTORAGE_CONFIGURATION1;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// VTable-based interface definitions (matching COM ABI layout)
// Each interface starts with a pointer to its vtable.
// The first 3 vtable entries are IUnknown: QueryInterface, AddRef, Release.
// ---------------------------------------------------------------------------

// IDStorageFile vtable
typedef struct DStorageFileVtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void (*Close)(struct DStorageFile* self);
    int32_t (*GetFileInformation)(struct DStorageFile* self, void* info);
} DStorageFileVtbl;

struct DStorageFile {
    const DStorageFileVtbl* lpVtbl;
};

// IDStorageStatusArray vtable
typedef struct DStorageStatusArrayVtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    bool (*IsComplete)(struct DStorageStatusArray* self, uint32_t index);
    int32_t (*GetHResult)(struct DStorageStatusArray* self, uint32_t index);
} DStorageStatusArrayVtbl;

struct DStorageStatusArray {
    const DStorageStatusArrayVtbl* lpVtbl;
};

// IDStorageQueue vtable
typedef struct DStorageQueueVtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void (*EnqueueRequest)(struct DStorageQueue* self, const DSTORAGE_REQUEST* request);
    void (*EnqueueStatus)(struct DStorageQueue* self, struct DStorageStatusArray* statusArray, uint32_t index);
    void (*EnqueueSignal)(struct DStorageQueue* self, void* fence, uint64_t value);
    void (*Submit)(struct DStorageQueue* self);
    void (*CancelRequestsWithTag)(struct DStorageQueue* self, uint64_t mask, uint64_t value);
    void (*Close)(struct DStorageQueue* self);
    void* (*GetErrorEvent)(struct DStorageQueue* self);
    void (*RetrieveErrorRecord)(struct DStorageQueue* self, DSTORAGE_ERROR_RECORD* record);
    void (*Query)(struct DStorageQueue* self, DSTORAGE_QUEUE_INFO* info);
} DStorageQueueVtbl;

struct DStorageQueue {
    const DStorageQueueVtbl* lpVtbl;
};

// IDStorageQueue1 extends IDStorageQueue
typedef struct DStorageQueue1Vtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void (*EnqueueRequest)(struct DStorageQueue* self, const DSTORAGE_REQUEST* request);
    void (*EnqueueStatus)(struct DStorageQueue* self, struct DStorageStatusArray* statusArray, uint32_t index);
    void (*EnqueueSignal)(struct DStorageQueue* self, void* fence, uint64_t value);
    void (*Submit)(struct DStorageQueue* self);
    void (*CancelRequestsWithTag)(struct DStorageQueue* self, uint64_t mask, uint64_t value);
    void (*Close)(struct DStorageQueue* self);
    void* (*GetErrorEvent)(struct DStorageQueue* self);
    void (*RetrieveErrorRecord)(struct DStorageQueue* self, DSTORAGE_ERROR_RECORD* record);
    void (*Query)(struct DStorageQueue* self, DSTORAGE_QUEUE_INFO* info);
    void (*EnqueueSetEvent)(struct DStorageQueue* self, void* handle);
} DStorageQueue1Vtbl;

struct DStorageQueue1 {
    const DStorageQueue1Vtbl* lpVtbl;
};

// IDStorageQueue2 extends IDStorageQueue1
typedef struct DStorageQueue2Vtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void (*EnqueueRequest)(struct DStorageQueue* self, const DSTORAGE_REQUEST* request);
    void (*EnqueueStatus)(struct DStorageQueue* self, struct DStorageStatusArray* statusArray, uint32_t index);
    void (*EnqueueSignal)(struct DStorageQueue* self, void* fence, uint64_t value);
    void (*Submit)(struct DStorageQueue* self);
    void (*CancelRequestsWithTag)(struct DStorageQueue* self, uint64_t mask, uint64_t value);
    void (*Close)(struct DStorageQueue* self);
    void* (*GetErrorEvent)(struct DStorageQueue* self);
    void (*RetrieveErrorRecord)(struct DStorageQueue* self, DSTORAGE_ERROR_RECORD* record);
    void (*Query)(struct DStorageQueue* self, DSTORAGE_QUEUE_INFO* info);
    void (*EnqueueSetEvent)(struct DStorageQueue* self, void* handle);
    DSTORAGE_COMPRESSION_SUPPORT (*GetCompressionSupport)(struct DStorageQueue* self, DSTORAGE_COMPRESSION_FORMAT format);
} DStorageQueue2Vtbl;

struct DStorageQueue2 {
    const DStorageQueue2Vtbl* lpVtbl;
};

// IDStorageQueue3 extends IDStorageQueue2
typedef struct DStorageQueue3Vtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void (*EnqueueRequest)(struct DStorageQueue* self, const DSTORAGE_REQUEST* request);
    void (*EnqueueStatus)(struct DStorageQueue* self, struct DStorageStatusArray* statusArray, uint32_t index);
    void (*EnqueueSignal)(struct DStorageQueue* self, void* fence, uint64_t value);
    void (*Submit)(struct DStorageQueue* self);
    void (*CancelRequestsWithTag)(struct DStorageQueue* self, uint64_t mask, uint64_t value);
    void (*Close)(struct DStorageQueue* self);
    void* (*GetErrorEvent)(struct DStorageQueue* self);
    void (*RetrieveErrorRecord)(struct DStorageQueue* self, DSTORAGE_ERROR_RECORD* record);
    void (*Query)(struct DStorageQueue* self, DSTORAGE_QUEUE_INFO* info);
    void (*EnqueueSetEvent)(struct DStorageQueue* self, void* handle);
    DSTORAGE_COMPRESSION_SUPPORT (*GetCompressionSupport)(struct DStorageQueue* self, DSTORAGE_COMPRESSION_FORMAT format);
    void (*EnqueueRequests)(struct DStorageQueue* self, const DSTORAGE_REQUEST* requests, uint32_t numRequests, void* fence, uint64_t value, DSTORAGE_ENQUEUE_REQUEST_FLAGS flags);
} DStorageQueue3Vtbl;

struct DStorageQueue3 {
    const DStorageQueue3Vtbl* lpVtbl;
};

// IDStorageFactory vtable
typedef struct DStorageFactoryVtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    int32_t (*CreateQueue)(struct DStorageFactory* self, const DSTORAGE_QUEUE_DESC* desc, const void* riid, void** ppv);
    int32_t (*OpenFile)(struct DStorageFactory* self, const uint16_t* path, const void* riid, void** ppv);
    int32_t (*CreateStatusArray)(struct DStorageFactory* self, uint32_t capacity, const char* name, const void* riid, void** ppv);
    void (*SetDebugFlags)(struct DStorageFactory* self, uint32_t flags);
    int32_t (*SetStagingBufferSize)(struct DStorageFactory* self, uint32_t size);
} DStorageFactoryVtbl;

struct DStorageFactory {
    const DStorageFactoryVtbl* lpVtbl;
};

// IDStorageCustomDecompressionQueue vtable
typedef struct DStorageCustomDecompressionQueueVtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void* (*GetEvent)(struct DStorageCustomDecompressionQueue* self);
    int32_t (*GetRequests)(struct DStorageCustomDecompressionQueue* self, uint32_t maxRequests, DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST* requests, uint32_t* numRequests);
    int32_t (*SetRequestResults)(struct DStorageCustomDecompressionQueue* self, uint32_t numResults, const DSTORAGE_CUSTOM_DECOMPRESSION_RESULT* results);
} DStorageCustomDecompressionQueueVtbl;

struct DStorageCustomDecompressionQueue {
    const DStorageCustomDecompressionQueueVtbl* lpVtbl;
};

// IDStorageCustomDecompressionQueue1 extends IDStorageCustomDecompressionQueue
typedef struct DStorageCustomDecompressionQueue1Vtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    void* (*GetEvent)(struct DStorageCustomDecompressionQueue* self);
    int32_t (*GetRequests)(struct DStorageCustomDecompressionQueue* self, uint32_t maxRequests, DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST* requests, uint32_t* numRequests);
    int32_t (*SetRequestResults)(struct DStorageCustomDecompressionQueue* self, uint32_t numResults, const DSTORAGE_CUSTOM_DECOMPRESSION_RESULT* results);
    int32_t (*GetRequests1)(struct DStorageCustomDecompressionQueue* self, DSTORAGE_GET_REQUEST_FLAGS flags, uint32_t maxRequests, DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST* requests, uint32_t* numRequests);
} DStorageCustomDecompressionQueue1Vtbl;

struct DStorageCustomDecompressionQueue1 {
    const DStorageCustomDecompressionQueue1Vtbl* lpVtbl;
};

// IDStorageCompressionCodec vtable
typedef struct DStorageCompressionCodecVtbl {
    int32_t (*QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (*AddRef)(void* self);
    uint32_t (*Release)(void* self);
    int32_t (*CompressBuffer)(struct DStorageCompressionCodec* self, const void* uncompressedData, size_t uncompressedDataSize, DSTORAGE_COMPRESSION compressionSetting, void* compressedBuffer, size_t compressedBufferSize, size_t* compressedDataSize);
    int32_t (*DecompressBuffer)(struct DStorageCompressionCodec* self, const void* compressedData, size_t compressedDataSize, void* uncompressedBuffer, size_t uncompressedBufferSize, size_t* uncompressedDataSize);
    size_t (*CompressBufferBound)(struct DStorageCompressionCodec* self, size_t uncompressedDataSize);
} DStorageCompressionCodecVtbl;

struct DStorageCompressionCodec {
    const DStorageCompressionCodecVtbl* lpVtbl;
};

// ---------------------------------------------------------------------------
// Exported functions (exact DirectStorage API names)
// ---------------------------------------------------------------------------
int32_t DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration);
int32_t DStorageSetConfiguration1(const DSTORAGE_CONFIGURATION1* configuration);
int32_t DStorageGetFactory(const void* riid, void** ppv);
int32_t DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT format, uint32_t numThreads, const void* riid, void** ppv);
void DStorageSetConfigurationSDK(DSTORAGE_CONFIGURATION const* configuration);

/// Non-standard extension: block until all in-flight requests on the queue
/// have completed. This is needed because the eventfd-based completion
/// mechanism is not wired through EnqueueSignal in this implementation.
int32_t DStorageQueueWait(DStorageQueue* queue);

#ifdef __cplusplus
}
#endif
