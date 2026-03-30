/* platform.c — platform capability detection (AVX2, io_uring, inotify) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * platform.c — runtime detection of Linux-specific features.
 *
 * Detects:
 *   - io_uring via SYS_io_uring_setup (no liburing dependency)
 *   - inotify  via inotify_init1()
 *
 * Both probes are conservative: on any error the feature is marked
 * unavailable and all code paths must fall back gracefully.
 */

#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* sys/syscall.h gives us SYS_io_uring_setup */
#include <sys/syscall.h>

/* syscall(2) is not declared in strict POSIX mode; declare it explicitly */
extern long syscall(long number, ...);

/* inotify */
#include <sys/inotify.h>

int g_uring_available    = 0;
int g_inotify_available  = 0;

/*
 * io_uring_params layout matches the kernel ABI.
 * We define the minimal struct here to avoid depending on linux/io_uring.h,
 * which may not be present on all toolchains.
 *
 * The kernel ABI is stable; only the first few fields matter for a
 * zero-initialised probe call.
 */
#ifndef SYS_io_uring_setup
/* Architecture-specific syscall numbers — Linux stable ABI */
#  if defined(__x86_64__)
#    define SYS_io_uring_setup 425
#  elif defined(__aarch64__)
#    define SYS_io_uring_setup 425
#  elif defined(__arm__)
#    define SYS_io_uring_setup 425
#  elif defined(__riscv)
#    define SYS_io_uring_setup 425
#  else
/* Unknown arch: disable io_uring */
#    define SYS_io_uring_setup (-1L)
#  endif
#endif

/*
 * Minimal io_uring_params: the kernel fills this in on io_uring_setup().
 * We zero-initialise it and only care whether the syscall succeeds.
 */
struct matchbox_io_uring_params {
    unsigned int sq_entries;
    unsigned int cq_entries;
    unsigned int flags;
    unsigned int sq_thread_cpu;
    unsigned int sq_thread_idle;
    unsigned int features;
    unsigned int wq_fd;
    unsigned int resv[3];
    /* sq_off and cq_off follow; we don't need them for the probe */
    unsigned char pad[128];
};

void platform_detect(void)
{
    /* MATCHBOX_FORCE_FALLBACKS=1: disable all optional kernel features.
     * Used for testing the portable fallback paths. */
    if (getenv("MATCHBOX_FORCE_FALLBACKS") != NULL) {
        g_uring_available   = 0;
        g_inotify_available = 0;
        return;
    }

    /* ------------------------------------------------------------------ */
    /* io_uring probe                                                       */
    /* ------------------------------------------------------------------ */
#if SYS_io_uring_setup != (-1L)
    {
        struct matchbox_io_uring_params params;
        memset(&params, 0, sizeof(params));

        long fd = syscall((long)SYS_io_uring_setup, (long)1, &params);
        if (fd >= 0) {
            g_uring_available = 1;
            close((int)fd);
        } else {
            /* ENOSYS: kernel too old; EPERM: seccomp/capability denied.
             * Either way, io_uring is not available. */
            g_uring_available = 0;
        }
    }
#else
    g_uring_available = 0;
#endif

    /* ------------------------------------------------------------------ */
    /* inotify probe                                                        */
    /* ------------------------------------------------------------------ */
    {
        int ifd = inotify_init1(IN_CLOEXEC);
        if (ifd >= 0) {
            g_inotify_available = 1;
            close(ifd);
        } else {
            g_inotify_available = 0;
        }
    }
}
