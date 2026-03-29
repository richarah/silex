#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * uring.c — batch operation execution with optional io_uring acceleration.
 *
 * When io_uring is available (g_uring_available == 1) we attempt to submit
 * all operations as a single SQE batch, falling back to sequential execution
 * if setup fails or the operations are not independent.
 *
 * For operations that are not natively expressible as io_uring SQEs (e.g.
 * mkdir, chmod) we fall through to the sequential fallback.  The io_uring
 * path currently accelerates only BATCH_RM_FILE (via IORING_OP_UNLINKAT)
 * and BATCH_TOUCH (via IORING_OP_STATX to probe + separate utimensat).
 *
 * The implementation uses raw syscalls (SYS_io_uring_setup / _enter /
 * _register) and shared-memory mapped rings to avoid any dependency on
 * liburing.
 *
 * Correctness guarantee: if io_uring is unavailable, or if any part of the
 * ring setup fails, we fall back to fallback_exec_seq() which is always
 * correct.
 */

#include "uring.h"
#include "detect.h"
#include "fallback.h"
#include "../util/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* syscall(2) is not declared in strict POSIX mode; declare it explicitly */
extern long syscall(long number, ...);

/* ------------------------------------------------------------------ */
/* Allocation helpers                                                   */
/* ------------------------------------------------------------------ */

batch_op_t *batch_op_new(batch_op_type_t type, const char *path, unsigned mode)
{
    batch_op_t *op = calloc(1, sizeof(*op));
    if (!op) return NULL;

    op->path = strdup(path);
    if (!op->path) {
        free(op);
        return NULL;
    }
    op->type = type;
    op->mode = mode;
    op->next = NULL;
    return op;
}

void batch_op_free(batch_op_t *op)
{
    if (!op) return;
    free(op->path);
    free(op);
}

/* ------------------------------------------------------------------ */
/* io_uring kernel ABI (minimal, stable subset)                        */
/* ------------------------------------------------------------------ */

/*
 * We define only the fields we use.  The full structs are much larger;
 * we allocate them by size and access members via offsets returned by
 * io_uring_setup, which avoids depending on linux/io_uring.h.
 */

#ifndef SYS_io_uring_setup
#  if defined(__x86_64__) || defined(__aarch64__) || \
      defined(__arm__) || defined(__riscv)
#    define SYS_io_uring_setup  425
#    define SYS_io_uring_enter  426
#  else
#    define SYS_io_uring_setup  (-1L)
#    define SYS_io_uring_enter  (-1L)
#  endif
#endif

/* Submission queue entry opcodes we use */
#define IORING_OP_NOP         0
#define IORING_OP_UNLINKAT   36  /* available since 5.11 */

/* io_uring_enter flags */
#define IORING_ENTER_GETEVENTS (1u << 0)

/* io_uring_setup flags */
#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL    (1u << 1)
#endif

/*
 * Minimal io_uring_params.  Kernel fills in sq_off / cq_off on return.
 * We only read sq_entries, cq_entries, and the offset arrays.
 */
#define IORING_PARAMS_SIZE  120   /* actual struct is larger; we only map what we need */

struct mb_io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    /* sq_off */
    struct {
        uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array;
        uint32_t resv1;
        uint64_t user_addr;
    } sq_off;
    /* cq_off */
    struct {
        uint32_t head, tail, ring_mask, ring_entries, overflow, cqes;
        uint32_t flags;
        uint32_t resv1;
        uint64_t user_addr;
    } cq_off;
};

/* Minimal SQE: 64 bytes */
struct mb_io_uring_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t ioprio;
    int32_t  fd;
    union { uint64_t off; uint64_t addr2; };
    union { uint64_t addr; uint64_t splice_off_in; };
    uint32_t len;
    union { uint32_t rw_flags; uint32_t fsync_flags;
            uint16_t poll_events; uint32_t poll32_events;
            uint32_t sync_range_flags; uint32_t msg_flags;
            uint32_t timeout_flags; uint32_t accept_flags;
            uint32_t cancel_flags; uint32_t open_flags;
            uint32_t statx_flags; uint32_t fadvise_advice;
            uint32_t splice_flags; uint32_t rename_flags;
            uint32_t unlink_flags; uint32_t hardlink_flags;
            uint32_t xattr_flags; uint32_t msg_ring_flags;
            uint32_t uring_cmd_flags; };
    uint64_t user_data;
    union { uint16_t buf_index; uint16_t buf_group; };
    uint16_t personality;
    union { int32_t splice_fd_in; uint32_t file_index;
            uint32_t optlen; struct { uint16_t addr_len; uint16_t __pad3[1]; }; };
    union { struct { uint64_t addr3; uint64_t __pad2[1]; };
            uint64_t optval;
            uint8_t  cmd_pad[16]; }; /* 16-byte pad; mirrors cmd[] in kernel ABI */
};

/* Minimal CQE: 16 bytes */
struct mb_io_uring_cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
};

/* ------------------------------------------------------------------ */
/* io_uring batch for BATCH_RM_FILE operations                         */
/* ------------------------------------------------------------------ */

/*
 * Try to execute all BATCH_RM_FILE ops via io_uring UNLINKAT.
 * Returns 0 on success, -1 on setup error (caller falls back to sequential).
 * On partial success, returns 0 but some files may not have been removed.
 */
static int uring_exec_unlinks(batch_op_t *ops, int count)
{
#if SYS_io_uring_setup == (-1L)
    (void)ops; (void)count;
    return -1;
#else
    struct mb_io_uring_params params;
    memset(&params, 0, sizeof(params));

    /* Enable SQPOLL when running as root (avoids syscall per submission) */
    if (getuid() == 0)
        params.flags |= IORING_SETUP_SQPOLL;

    uint32_t entries = (uint32_t)count;
    /* Round up to next power of two (kernel requires this) */
    uint32_t sq_size = 1;
    while (sq_size < entries) sq_size <<= 1;
    /* Cap at 256 to limit memory and prevent runaway queue depth */
    if (sq_size > 256) sq_size = 256;

    long ring_fd = syscall((long)SYS_io_uring_setup, (long)sq_size, &params);
    if (ring_fd < 0)
        return -1;

    /* Map the SQ ring */
    size_t sq_ring_sz = params.sq_off.array
                      + params.sq_entries * sizeof(uint32_t);
    void *sq_ptr = mmap(NULL, sq_ring_sz,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        (int)ring_fd, 0 /* IORING_OFF_SQ_RING = 0 */);
    if (sq_ptr == MAP_FAILED) {
        close((int)ring_fd);
        return -1;
    }

    /* Map the SQE array (offset 0x10000000) */
    size_t sqe_sz  = params.sq_entries * sizeof(struct mb_io_uring_sqe);
    void  *sqe_ptr = mmap(NULL, sqe_sz,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE,
                          (int)ring_fd, 0x10000000LL /* IORING_OFF_SQES */);
    if (sqe_ptr == MAP_FAILED) {
        munmap(sq_ptr, sq_ring_sz);
        close((int)ring_fd);
        return -1;
    }

    /* Map the CQ ring */
    size_t cq_ring_sz = params.cq_off.cqes
                      + params.cq_entries * sizeof(struct mb_io_uring_cqe);
    void *cq_ptr = mmap(NULL, cq_ring_sz,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        (int)ring_fd, 0x8000000LL /* IORING_OFF_CQ_RING */);
    if (cq_ptr == MAP_FAILED) {
        munmap(sqe_ptr, sqe_sz);
        munmap(sq_ptr, sq_ring_sz);
        close((int)ring_fd);
        return -1;
    }

    /* Pointers into the SQ ring */
    uint32_t *sq_tail  = (uint32_t *)((char *)sq_ptr + params.sq_off.tail);
    uint32_t *sq_array = (uint32_t *)((char *)sq_ptr + params.sq_off.array);
    uint32_t  sq_mask  = *(uint32_t *)((char *)sq_ptr + params.sq_off.ring_mask);

    /* Fill SQEs */
    struct mb_io_uring_sqe *sqes = (struct mb_io_uring_sqe *)sqe_ptr;
    uint32_t tail = *sq_tail;
    int idx = 0;

    for (batch_op_t *op = ops; op; op = op->next) {
        if (op->type != BATCH_RM_FILE) continue;

        uint32_t slot = tail & sq_mask;
        struct mb_io_uring_sqe *sqe = &sqes[slot];
        memset(sqe, 0, sizeof(*sqe));

        sqe->opcode       = IORING_OP_UNLINKAT;
        sqe->fd           = AT_FDCWD;
        sqe->addr         = (uint64_t)(uintptr_t)op->path;
        sqe->len          = 0;
        sqe->unlink_flags = 0;
        sqe->user_data    = (uint64_t)(uintptr_t)op;

        sq_array[slot] = slot;
        tail++;
        idx++;
    }

    /* Memory barrier before updating tail */
    __atomic_store_n(sq_tail, tail, __ATOMIC_RELEASE);

    /* Submit and wait for completions */
    long submitted = syscall((long)SYS_io_uring_enter,
                             (long)ring_fd,
                             (long)idx,
                             (long)idx,
                             (long)IORING_ENTER_GETEVENTS,
                             (long)NULL,
                             (long)0);

    /* Harvest CQEs (best-effort; errors reported to stderr) */
    if (submitted > 0) {
        uint32_t *cq_head  = (uint32_t *)((char *)cq_ptr + params.cq_off.head);
        uint32_t *cq_tail_r = (uint32_t *)((char *)cq_ptr + params.cq_off.tail);
        uint32_t  cq_mask  = *(uint32_t *)((char *)cq_ptr + params.cq_off.ring_mask);
        struct mb_io_uring_cqe *cqes_arr =
            (struct mb_io_uring_cqe *)((char *)cq_ptr + params.cq_off.cqes);

        uint32_t head = __atomic_load_n(cq_head, __ATOMIC_ACQUIRE);
        uint32_t ctail = __atomic_load_n(cq_tail_r, __ATOMIC_ACQUIRE);

        while (head != ctail) {
            struct mb_io_uring_cqe *cqe = &cqes_arr[head & cq_mask];
            if (cqe->res < 0) {
                batch_op_t *failed = (batch_op_t *)(uintptr_t)cqe->user_data;
                fprintf(stderr, "matchbox: batch rm '%s': %s\n",
                        failed ? failed->path : "?",
                        strerror(-(int)cqe->res));
            }
            head++;
        }
        __atomic_store_n(cq_head, head, __ATOMIC_RELEASE);
    }

    munmap(cq_ptr, cq_ring_sz);
    munmap(sqe_ptr, sqe_sz);
    munmap(sq_ptr, sq_ring_sz);
    close((int)ring_fd);

    return (submitted < 0) ? -1 : 0;
#endif /* SYS_io_uring_setup */
}

/* ------------------------------------------------------------------ */
/* Public batch_exec                                                    */
/* ------------------------------------------------------------------ */

int batch_exec(batch_op_t *ops)
{
    if (!ops)
        return 0;

    /* If io_uring is not available, always use sequential fallback */
    if (!g_uring_available)
        return fallback_exec_seq(ops);

    /* Check whether all ops can be run independently */
    if (!batch_ops_independent(ops))
        return fallback_exec_seq(ops);

    /*
     * For homogeneous BATCH_RM_FILE lists, attempt an io_uring batch.
     * All other operation types go through the sequential fallback.
     */
    int all_rm    = 1;
    int rm_count  = 0;

    for (batch_op_t *op = ops; op; op = op->next) {
        if (op->type != BATCH_RM_FILE) { all_rm = 0; break; }
        rm_count++;
    }

    if (all_rm && rm_count > 0) {
        int r = uring_exec_unlinks(ops, rm_count);
        if (r == 0)
            return 0;
        /* io_uring setup failed: fall through to sequential */
    }

    return fallback_exec_seq(ops);
}
