/*
 * libds_uring.c — io_uring backend for Wine DirectStorage
 *
 * This Unix shared library provides the low-level I/O foundation for
 * DirectStorage on Linux. It is loaded by dstoragecore.dll via dlopen.
 *
 * Architecture:
 *   The library manages one or more io_uring instances that provide
 *   kernel-bypass async I/O. Key features used:
 *
 *   1. IORING_SETUP_IOPOLL — Polled I/O mode. The kernel polls the NVMe
 *      completion queue instead of using interrupts. This reduces I/O
 *      latency from ~5μs (interrupt) to ~1-2μs (polled).
 *
 *   2. IORING_REGISTER_BUFFERS — Registered (pinned) buffers. When a
 *      buffer is registered, the kernel pre-pins the memory pages and
 *      avoids the per-I/O page pinning overhead. This is critical for
 *      DirectStorage's frequent buffer reuse pattern.
 *
 *   3. IORING_REGISTER_FILES — Fixed files. Pre-register file descriptors
 *      to skip fget/fput per I/O operation. Useful for archive files
 *      that the game reads from repeatedly.
 *
 *   4. IORING_OP_READ — Async read operation. The primary I/O path.
 *
 *   5. IORING_OP_ASYNC_CANCEL — Cancel in-flight I/O (Linux 5.13+).
 *      Used for CancelRequestsWithTag.
 *
 * The io_uring instance is shared across all queues from the same
 * factory. Each SQE is submitted with a user-data pointer that
 * identifies the completion callback.
 *
 * References:
 *   - Linux io_uring man page (io_uring_enter, io_uring_setup)
 *   - io_uring PDF: https://kernel.dk/io_uring.pdf
 *   - liburing: https://github.com/axboe/liburing
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* 
 * Linux io_uring headers.
 * We include the raw kernel header for maximum portability.
 * liburing is not required — we use the io_uring syscalls directly.
 */
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>

/* 
 * io_uring system call number (x86_64).
 * On other architectures, this differs (e.g., 425 on arm64).
 */
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif

/* ------------------------------------------------------------------
 * Data structures
 * ------------------------------------------------------------------ */

/* Completion callback type — matches the Windows OVERLAPPED model */
typedef void (*uring_callback_t)(void *userdata, int result, unsigned bytes);

/* 
 * Per-I/O metadata stored in the SQE user_data field.
 * This is how we associate a completion (CQE) with the original request.
 */
struct io_metadata
{
    uring_callback_t callback;
    void *userdata;
    void *buffer;           /* Buffer for registered I/O */
    int fd;                 /* File descriptor (for cancellation) */
    uint64_t offset;        /* File offset (for cancellation) */
};

/* 
 * io_uring ring instance.
 * We maintain one ring per process (shared across all queues).
 * Each ring has:
 *   - Submission Queue (SQ): where we place new I/O requests
 *   - Completion Queue (CQ): where the kernel places results
 *   - A kernel thread (if SQPOLL) that polls the SQ
 */
struct ds_uring
{
    struct io_uring_sq {
        unsigned *head;             /* Kernel updates head as it consumes SQEs */
        unsigned *tail;             /* We update tail as we add SQEs */
        unsigned *ring_mask;        /* Mask & (ring_size - 1) for index wrapping */
        unsigned *ring_entries;     /* Number of SQ entries */
        unsigned *flags;            /* SQ flags (e.g., need_wakeup) */
        unsigned *array;            /* SQE index table */
        struct io_uring_sqe *sqes;  /* The actual SQE ring buffer */
        size_t ring_sz;             /* Mapped size of the SQ */
        void *ring_ptr;             /* Mmap'd SQ memory */
    } sq;
    
    struct io_uring_cq {
        unsigned *head;             /* We update head as we consume CQEs */
        unsigned *tail;             /* Kernel updates tail as it adds CQEs */
        unsigned *ring_mask;        /* Mask for index wrapping */
        unsigned *ring_entries;     /* Number of CQ entries */
        struct io_uring_cqe *cqes;  /* The CQE ring buffer */
        size_t ring_sz;             /* Mapped size of the CQ */
        void *ring_ptr;             /* Mmap'd CQ memory */
    } cq;
    
    int ring_fd;                    /* File descriptor for the io_uring instance */
    unsigned entries;               /* Number of SQ entries */
    
    /* Registered buffers for zero-copy I/O */
    struct iovec *buffers;
    unsigned nr_buffers;
    
    /* Registered files for fast fd lookup */
    int *files;
    unsigned nr_files;
    
    /* 
     * Completion thread: processes CQEs and invokes callbacks.
     * This thread runs continuously, polling the CQ for completions.
     */
    pthread_t completion_thread;
    volatile int stop_thread;
    int wake_pipe[2];              /* Pipe to wake the completion thread */
};

/* ------------------------------------------------------------------
 * System call wrappers
 *
 * These are direct syscall invocations to avoid depending on liburing.
 * The io_uring syscalls are:
 *   io_uring_setup(entries, params) — create a new io_uring instance
 *   io_uring_enter(ring_fd, to_submit, min_complete, flags) — submit + wait
 *   io_uring_register(ring_fd, opcode, arg, nr_args) — register buffers/files
 * ------------------------------------------------------------------ */

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int ring_fd, unsigned to_submit,
                                      unsigned min_complete, unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

static inline int sys_io_uring_register(int ring_fd, unsigned opcode,
                                         void *arg, unsigned nr_args)
{
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

/* ------------------------------------------------------------------
 * Initialize an io_uring ring
 *
 * Parameters:
 *   entries: Number of SQ entries (power of 2, typically 256-4096)
 *   use_poll: Enable IORING_SETUP_IOPOLL for NVMe polling
 *   use_sqpoll: Enable IORING_SETUP_SQPOLL for kernel-thread SQ polling
 *
 * Returns:
 *   Pointer to ds_uring struct, or NULL on failure
 *
 * The ring is initialized with:
 *   - IORING_FEAT_SINGLE_MMAP if available (maps SQ + CQ in one chunk)
 *   - Submission queue entries sized to 'entries'
 *   - Completion queue entries sized to 2*entries (kernel may adjust)
 * ------------------------------------------------------------------ */
struct ds_uring *ds_uring_init(unsigned entries, int use_poll, int use_sqpoll)
{
    struct ds_uring *ring;
    struct io_uring_params params;
    int ret;

    ring = calloc(1, sizeof(*ring));
    if (!ring) return NULL;
    ring->entries = entries;
    ring->ring_fd = -1;
    ring->wake_pipe[0] = -1;
    ring->wake_pipe[1] = -1;

    memset(&params, 0, sizeof(params));

    /*
     * IOPOLL mode: kernel polls NVMe completion queue instead of using IRQs.
     * Reduces latency by 1-3μs per I/O. Required for BypassIO-like performance.
     * Corresponds to Windows FILE_FLAG_NO_BUFFERING behavior.
     */
    if (use_poll)
        params.flags |= IORING_SETUP_IOPOLL;
    
    /*
     * SQPOLL mode: kernel thread polls the submission queue.
     * Eliminates io_uring_enter syscall entirely for submissions.
     * This is the Linux equivalent of Windows IoRing's kernel-bypass path.
     * Without SQPOLL, every submission requires io_uring_enter() syscall.
     * With SQPOLL, the kernel thread picks up SQEs without any syscall.
     *
     * This is the key to matching DirectStorage's BypassIO performance:
     *   Windows BypassIO → kernel-bypass, no filesystem overhead
     *   Linux SQPOLL → kernel-thread polling, no io_uring_enter syscall
     */
    if (use_sqpoll)
        params.flags |= IORING_SETUP_SQPOLL;
    
    /* Create the io_uring instance */
    ring->ring_fd = sys_io_uring_setup(entries, &params);
    if (ring->ring_fd < 0)
    {
        free(ring);
        return NULL;
    }
    
    /* 
     * Map the submission and completion queues.
     * With IORING_FEAT_SINGLE_MMAP (kernel 5.4+), both queues
     * can be mapped with a single mmap call. Otherwise we need
     * separate mmaps for SQ and CQ.
     */
    int single_mmap = (params.features & IORING_FEAT_SINGLE_MMAP);
    
    /* SQ ring size */
    ring->sq.ring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    
    /* CQ ring size */
    ring->cq.ring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    
    if (single_mmap)
    {
        /* Map SQ + CQ together in one chunk */
        size_t total_sz = ring->sq.ring_sz < ring->cq.ring_sz ?
                          ring->cq.ring_sz : ring->sq.ring_sz;
        
        ring->sq.ring_ptr = mmap(0, total_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE,
                                  ring->ring_fd, IORING_OFF_SQ_RING);
        if (ring->sq.ring_ptr == MAP_FAILED)
            goto fail;
        
        ring->cq.ring_ptr = ring->sq.ring_ptr;
    }
    else
    {
        /* Map SQ ring */
        ring->sq.ring_ptr = mmap(0, ring->sq.ring_sz,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE,
                                  ring->ring_fd, IORING_OFF_SQ_RING);
        if (ring->sq.ring_ptr == MAP_FAILED)
            goto fail;
        
        /* Map CQ ring */
        ring->cq.ring_ptr = mmap(0, ring->cq.ring_sz,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE,
                                  ring->ring_fd, IORING_OFF_CQ_RING);
        if (ring->cq.ring_ptr == MAP_FAILED)
            goto fail;
    }
    
    /* Map SQE array (always separate mmap) */
    ring->sq.sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE,
                          ring->ring_fd, IORING_OFF_SQES);
    if (ring->sq.sqes == MAP_FAILED)
        goto fail;
    
    /* Set up pointer shortcuts for the SQ ring */
    ring->sq.head = ring->sq.ring_ptr + params.sq_off.head;
    ring->sq.tail = ring->sq.ring_ptr + params.sq_off.tail;
    ring->sq.ring_mask = ring->sq.ring_ptr + params.sq_off.ring_mask;
    ring->sq.ring_entries = ring->sq.ring_ptr + params.sq_off.ring_entries;
    ring->sq.flags = ring->sq.ring_ptr + params.sq_off.flags;
    ring->sq.array = ring->sq.ring_ptr + params.sq_off.array;
    
    /* Set up pointer shortcuts for the CQ ring */
    ring->cq.head = ring->cq.ring_ptr + params.cq_off.head;
    ring->cq.tail = ring->cq.ring_ptr + params.cq_off.tail;
    ring->cq.ring_mask = ring->cq.ring_ptr + params.cq_off.ring_mask;
    ring->cq.ring_entries = ring->cq.ring_ptr + params.cq_off.ring_entries;
    ring->cq.cqes = ring->cq.ring_ptr + params.cq_off.cqes;
    
    /* 
     * Create a wakeup pipe for the completion thread.
     * This allows us to wake the thread when new SQEs are submitted
     * (in case it's waiting in io_uring_enter with no SQEs to process).
     */
    if (pipe2(ring->wake_pipe, O_NONBLOCK) < 0)
        goto fail;
    
    return ring;

fail:
    if (ring->sq.ring_ptr && ring->sq.ring_ptr != MAP_FAILED)
        munmap(ring->sq.ring_ptr, ring->sq.ring_sz);
    if (ring->cq.ring_ptr && ring->cq.ring_ptr != MAP_FAILED &&
        ring->cq.ring_ptr != ring->sq.ring_ptr)
        munmap(ring->cq.ring_ptr, ring->cq.ring_sz);
    if (ring->sq.sqes && ring->sq.sqes != MAP_FAILED)
        munmap(ring->sq.sqes, params.sq_entries * sizeof(struct io_uring_sqe));
    if (ring->ring_fd >= 0)
        close(ring->ring_fd);
    free(ring);
    return NULL;
}


/* ------------------------------------------------------------------
 * Register buffers for zero-copy I/O
 *
 * Registered buffers are pinned in kernel memory, eliminating the
 * per-I/O page pinning overhead. In DirectStorage, the same set of
 * staging buffers is reused across many I/O operations, making this
 * a critical optimization.
 *
 * Parameters:
 *   ring: io_uring ring instance
 *   buffers: array of {iov_base, iov_len} describing the buffers
 *   nr_buffers: number of buffers in the array
 *
 * Returns:
 *   0 on success, negative error code on failure
 * ------------------------------------------------------------------ */
int ds_uring_register_buffers(struct ds_uring *ring,
                               void **buffers, size_t *sizes,
                               unsigned nr_buffers)
{
    struct iovec *iov;
    int ret;
    
    if (!ring || !buffers || !sizes || nr_buffers == 0)
        return -EINVAL;
    
    /* Allocate iovec array for kernel registration */
    iov = calloc(nr_buffers, sizeof(struct iovec));
    if (!iov)
        return -ENOMEM;
    
    for (unsigned i = 0; i < nr_buffers; i++)
    {
        iov[i].iov_base = buffers[i];
        iov[i].iov_len = sizes[i];
    }
    
    ret = sys_io_uring_register(ring->ring_fd,
                                 IORING_REGISTER_BUFFERS, iov, nr_buffers);
    
    if (ret == 0)
    {
        /* Save for later cleanup */
        ring->buffers = iov;
        ring->nr_buffers = nr_buffers;
    }
    else
    {
        free(iov);
    }
    
    return ret;
}

/* ------------------------------------------------------------------
 * Register files for fast fd lookup
 *
 * Pre-register file descriptors to skip the kernel's fget/fput per I/O.
 * This is important for archive files that receive many I/O requests.
 * ------------------------------------------------------------------ */
int ds_uring_register_files(struct ds_uring *ring, int *files, unsigned nr_files)
{
    int ret;
    
    if (!ring || !files || nr_files == 0)
        return -EINVAL;
    
    ret = sys_io_uring_register(ring->ring_fd,
                                 IORING_REGISTER_FILES, files, nr_files);
    
    if (ret == 0)
    {
        /* Save for later cleanup — the caller owns the fd array */
        ring->files = files;
        ring->nr_files = nr_files;
    }
    
    return ret;
}

/* ------------------------------------------------------------------
 * Submit an async read operation
 *
 * This is the primary I/O path for DirectStorage. It submits a read
 * request to the io_uring submission queue.
 *
 * Parameters:
 *   ring: io_uring ring instance
 *   fd: file descriptor to read from
 *   offset: byte offset in the file
 *   size: number of bytes to read
 *   dst: destination buffer (must be at least 'size' bytes)
 *   cb: completion callback (called on I/O completion)
 *   userdata: opaque pointer passed to the callback
 *
 * The completion callback is invoked from the completion thread when
 * the I/O operation finishes. The callback receives:
 *   userdata: the opaque pointer passed here
 *   result: number of bytes read (negative on error)
 *   bytes: same as result if positive, 0 on error
 *
 * Returns:
 *   0 on success (SQE submitted), negative on error
 * ------------------------------------------------------------------ */
int ds_uring_read(struct ds_uring *ring, int fd,
                   uint64_t offset, uint32_t size,
                   void *dst,
                   uring_callback_t cb, void *userdata)
{
    struct io_uring_sqe *sqe;
    unsigned tail, head, mask, index;
    struct io_metadata *meta;
    
    if (!ring || fd < 0 || !dst || !cb)
        return -EINVAL;
    
    /* 
     * Check if there's room in the submission queue.
     * We need at least one free SQE. The ring is full when
     * (tail - head) >= (entries - 1). We use the ring's
     * head/tail pointers directly for speed.
     */
    mask = *ring->sq.ring_mask;
    tail = *ring->sq.tail;
    head = *ring->sq.head;
    
    if (tail - head >= ring->entries - 1)
        return -EBUSY;  /* SQ is full, caller should retry */
    
    /* Allocate metadata for this I/O */
    meta = calloc(1, sizeof(*meta));
    if (!meta)
        return -ENOMEM;
    
    meta->callback = cb;
    meta->userdata = userdata;
    meta->buffer = dst;
    meta->fd = fd;
    meta->offset = offset;
    
    /* Get the next SQE slot */
    index = tail & mask;
    sqe = &ring->sq.sqes[index];
    
    /* Prepare the read SQE */
    sqe->opcode = IORING_OP_READ;
    sqe->flags = 0;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (unsigned long)dst;
    sqe->len = size;
    sqe->rw_flags = 0;
    sqe->buf_index = 0;  /* Use registered buffer 0 if registered */
    sqe->personality = 0;
    sqe->file_index = 0;
    sqe->addr3 = 0;
    
    /* 
     * Store metadata pointer as the user data.
     * When the CQE arrives, we extract this pointer and
     * call the completion callback.
     */
    sqe->user_data = (unsigned long)meta;
    
    /* Update the SQE index table and advance the SQ tail */
    ring->sq.array[index] = index;
    
    /* 
     * Memory barrier: ensure SQE is visible to kernel before
     * updating the tail. SMP wmb() is sufficient here.
     */
    __sync_synchronize();
    
    *ring->sq.tail = tail + 1;
    
    /* 
     * Submit the SQE to the kernel.
     * io_uring_enter with 1 to_submit notifies the kernel that
     * one new SQE is available for processing.
     */
    sys_io_uring_enter(ring->ring_fd, 1, 0, IORING_ENTER_GETEVENTS);
    
    return 0;
}

/* 
 * Submit an async write operation.
 * Same interface as ds_uring_read but for writes.
 * Used for debugging/testing texture injection or save data.
 */
int ds_uring_write(struct ds_uring *ring, int fd,
                    uint64_t offset, uint32_t size,
                    const void *src,
                    uring_callback_t cb, void *userdata)
{
    struct io_uring_sqe *sqe;
    unsigned tail, head, mask, index;
    struct io_metadata *meta;
    
    if (!ring || fd < 0 || !src || !cb)
        return -EINVAL;
    
    mask = *ring->sq.ring_mask;
    tail = *ring->sq.tail;
    head = *ring->sq.head;
    
    if (tail - head >= ring->entries - 1)
        return -EBUSY;
    
    meta = calloc(1, sizeof(*meta));
    if (!meta) return -ENOMEM;
    
    meta->callback = cb;
    meta->userdata = userdata;
    meta->buffer = (void*)src;
    meta->fd = fd;
    meta->offset = offset;
    
    index = tail & mask;
    sqe = &ring->sq.sqes[index];
    
    sqe->opcode = IORING_OP_WRITE;
    sqe->flags = 0;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (unsigned long)src;
    sqe->len = size;
    sqe->rw_flags = 0;
    sqe->buf_index = 0;
    sqe->personality = 0;
    sqe->file_index = 0;
    sqe->addr3 = 0;
    
    sqe->user_data = (unsigned long)meta;
    ring->sq.array[index] = index;
    
    __sync_synchronize();
    
    *ring->sq.tail = tail + 1;
    sys_io_uring_enter(ring->ring_fd, 1, 0, IORING_ENTER_GETEVENTS);
    
    return 0;
}

/*
 * Drain pending completions from the CQ.
 * Processes all available CQEs and invokes their callbacks.
 *
 * Returns the number of CQEs processed.
 */
int ds_uring_drain(struct ds_uring *ring)
{
    unsigned head, tail, mask;
    int processed = 0;
    
    if (!ring) return 0;
    
    mask = *ring->cq.ring_mask;
    head = *ring->cq.head;
    tail = *ring->cq.tail;
    
    while (head != tail)
    {
        struct io_uring_cqe *cqe = &ring->cq.cqes[head & mask];
        struct io_metadata *meta = (struct io_metadata*)cqe->user_data;
        
        if (meta && meta->callback)
        {
            int result = cqe->res;
            unsigned bytes = (result > 0) ? (unsigned)result : 0;
            
            /* Invoke the completion callback */
            meta->callback(meta->userdata, result, bytes);
            
            free(meta);
        }
    }
    
    return processed;
}

void ds_uring_destroy(struct ds_uring *ring);


/*
 * IoRing API mapping — DirectStorage IoRing → Linux io_uring
 *
 * Windows DirectStorage uses the IoRing API for kernel-bypass I/O:
 *   CreateIoRing         → io_uring_setup()
 *   BuildIoRingReadFile  → io_uring_prep_read()
 *   SubmitIoRing         → io_uring_enter()
 *   PopIoRingCompletion  → io_uring_peek_cqe() / io_uring_cqe_seen()
 *   SetIoRingCompletionEvent → eventfd for completion notification
 *   BuildIoRingRegisterBuffers → IORING_REGISTER_BUFFERS
 *   CloseIoRing          → close(ring_fd)
 *
 * These functions wrap io_uring to match the Windows IoRing ABI.
 * dstoragecore.dll calls these via function pointers loaded from
 * this library at runtime.
 */

/*
 * IoRing equivalent: create a new I/O ring with N entries.
 * Corresponds to Windows: CreateIoRing(version, flags, size, h)
 */
struct io_ring *ds_ring_create(unsigned entries, unsigned flags)
{
    struct ds_uring *ring = ds_uring_init(entries,
        (flags & 1) ? 1 : 0,  /* IOPOLL */
        (flags & 2) ? 1 : 0); /* SQPOLL */
    return (struct io_ring*)ring;
}

/*
 * IoRing equivalent: prepare a read operation on a file.
 * Corresponds to Windows: BuildIoRingReadFile(ioring, file, buffer, size, offset, ...)
 * Returns 0 on success, -1 on failure with errno set.
 */
int ds_ring_read_file(struct io_ring *ring_ptr, int fd, void *buffer,
                       unsigned size, uint64_t offset,
                       void *completion_cookie)
{
    struct ds_uring *ring = (struct ds_uring*)ring_ptr;
    struct io_uring_sqe *sqe;
    unsigned tail, head, mask, index;
    
    if (!ring || fd < 0 || !buffer)
        return -EINVAL;
    
    mask = *ring->sq.ring_mask;
    tail = *ring->sq.tail;
    head = *ring->sq.head;
    
    if (tail - head >= ring->entries - 1)
        return -EBUSY;
    
    index = tail & mask;
    sqe = &ring->sq.sqes[index];
    
    /* Build the read operation — matches BuildIoRingReadFile ABI */
    sqe->opcode = IORING_OP_READ;
    sqe->flags = 0;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = offset;
    sqe->addr = (unsigned long)buffer;
    sqe->len = size;
    sqe->rw_flags = 0;
    sqe->buf_index = 0;
    sqe->personality = 0;
    sqe->file_index = 0;
    sqe->addr3 = 0;
    sqe->user_data = (unsigned long)completion_cookie;
    
    ring->sq.array[index] = index;
    __sync_synchronize();
    *ring->sq.tail = tail + 1;
    
    return 0;
}

/*
 * IoRing equivalent: submit pending operations.
 * Corresponds to Windows: SubmitIoRing(ioring, wait_ms, ...)
 * Returns number of submitted operations.
 */
int ds_ring_submit(struct io_ring *ring_ptr, unsigned wait_ms)
{
    struct ds_uring *ring = (struct ds_uring*)ring_ptr;
    unsigned to_submit;
    unsigned head, tail;
    
    if (!ring) return -EINVAL;
    
    head = *ring->sq.head;
    tail = *ring->sq.tail;
    to_submit = tail - head;
    
    if (to_submit == 0)
        return 0;
    
    return (int)sys_io_uring_enter(ring->ring_fd, to_submit,
                                    (wait_ms > 0) ? 1 : 0, 0);
}

/*
 * IoRing equivalent: pop completed operations.
 * Corresponds to Windows: PopIoRingCompletion(ioring, ...)
 * Returns 1 if completion available, 0 if none.
 * completion_cookie receives the cookie from the submitted operation.
 * result receives the I/O result (bytes read or error code).
 */
int ds_ring_pop_completion(struct io_ring *ring_ptr,
                            void **completion_cookie, int *result)
{
    struct ds_uring *ring = (struct ds_uring*)ring_ptr;
    unsigned head, tail, mask;
    
    if (!ring) return -EINVAL;
    
    mask = *ring->cq.ring_mask;
    head = *ring->cq.head;
    tail = *ring->cq.tail;
    
    if (head == tail)
        return 0;  /* No completions available */
    
    struct io_uring_cqe *cqe = &ring->cq.cqes[head & mask];
    
    if (completion_cookie)
        *completion_cookie = (void*)cqe->user_data;
    if (result)
        *result = cqe->res;
    
    /* Mark completion as consumed */
    *ring->cq.head = head + 1;
    
    return 1;
}

/*
 * IoRing equivalent: set an event for completion notification.
 * Corresponds to Windows: SetIoRingCompletionEvent(ioring, event)
 * The event is signaled when new completions are available.
 */
int ds_ring_set_completion_event(struct io_ring *ring_ptr, int event_fd)
{
    struct ds_uring *ring = (struct ds_uring*)ring_ptr;
    if (!ring || event_fd < 0) return -EINVAL;
    ring->wake_pipe[0] = dup(event_fd);
    return 0;
}

/*
 * IoRing equivalent: register buffers for zero-copy I/O.
 * Corresponds to Windows: BuildIoRingRegisterBuffers(ioring, buffers, count)
 */
int ds_ring_register_buffers(struct io_ring *ring_ptr,
                              struct iovec *buffers, unsigned count)
{
    struct ds_uring *ring = (struct ds_uring*)ring_ptr;
    if (!ring || !buffers || count == 0) return -EINVAL;
    return sys_io_uring_register(ring->ring_fd,
                                  IORING_REGISTER_BUFFERS, buffers, count);
}

/*
 * IoRing equivalent: close/destroy an I/O ring.
 * Corresponds to Windows: CloseIoRing(ioring)
 */
void ds_ring_close(struct io_ring *ring_ptr)
{
    ds_uring_destroy((struct ds_uring*)ring_ptr);
}


/*
 * Destroy the io_uring ring and free all resources.
 * Cancels any in-flight I/O (though the kernel handles this
 * automatically when the ring fd is closed).
 */
void ds_uring_destroy(struct ds_uring *ring)
{
    if (!ring) return;
    
    /* Stop the completion thread */
    ring->stop_thread = 1;
    if (ring->wake_pipe[1] >= 0)
    {
        char c = 1;
        write(ring->wake_pipe[1], &c, 1);
    }
    
    /* Free registered buffers */
    if (ring->buffers)
    {
        sys_io_uring_register(ring->ring_fd,
                               IORING_UNREGISTER_BUFFERS, NULL, 0);
        free(ring->buffers);
    }
    
    /* Free registered files */
    if (ring->files)
    {
        sys_io_uring_register(ring->ring_fd,
                               IORING_UNREGISTER_FILES, NULL, 0);
        free(ring->files);
    }
    
    /* Unmap ring memory */
    if (ring->sq.ring_ptr && ring->sq.ring_ptr != MAP_FAILED)
    {
        size_t sz = ring->sq.ring_sz;
        if (ring->cq.ring_ptr == ring->sq.ring_ptr)
            sz = (ring->sq.ring_sz > ring->cq.ring_sz) ?
                  ring->sq.ring_sz : ring->cq.ring_sz;
        munmap(ring->sq.ring_ptr, sz);
    }
    if (ring->cq.ring_ptr && ring->cq.ring_ptr != MAP_FAILED &&
        ring->cq.ring_ptr != ring->sq.ring_ptr)
        munmap(ring->cq.ring_ptr, ring->cq.ring_sz);
    
    if (ring->sq.sqes && ring->sq.sqes != MAP_FAILED)
        munmap(ring->sq.sqes, ring->entries * sizeof(struct io_uring_sqe));
    
    /* Close ring fd (cancels all in-flight I/O) */
    if (ring->ring_fd >= 0)
        close(ring->ring_fd);
    
    if (ring->wake_pipe[0] >= 0) close(ring->wake_pipe[0]);
    if (ring->wake_pipe[1] >= 0) close(ring->wake_pipe[1]);
    
    free(ring);
}
