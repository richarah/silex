/* fallback.c — portable I/O fallback implementations (read/write/splice) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * fallback.c — sequential execution of batch operations.
 *
 * This is the always-correct fallback path used when io_uring is
 * unavailable, the operations are not independent, or the uring path
 * fails at setup time.
 *
 * Supported operations:
 *   BATCH_MKDIR   — mkdir(path, mode)
 *   BATCH_CHMOD   — chmod(path, mode)
 *   BATCH_TOUCH   — utimensat(AT_FDCWD, path, NULL, 0)  [set to current time]
 *   BATCH_RM_FILE — unlink(path)
 */

#include "fallback.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* utimensat is declared in <sys/stat.h> with _POSIX_C_SOURCE 200809L */
#include <fcntl.h>   /* AT_FDCWD */

int fallback_exec_seq(batch_op_t *ops)
{
    int ret = 0;

    for (batch_op_t *op = ops; op; op = op->next) {
        if (!op->path) {
            fprintf(stderr, "matchbox: batch op with NULL path; skipping\n");
            ret = 1;
            continue;
        }

        switch (op->type) {

        case BATCH_MKDIR:
            if (mkdir(op->path, (mode_t)op->mode) != 0) {
                fprintf(stderr, "matchbox: batch mkdir '%s': %s\n",
                        op->path, strerror(errno));
                ret = 1;
            }
            break;

        case BATCH_CHMOD:
            if (chmod(op->path, (mode_t)op->mode) != 0) {
                fprintf(stderr, "matchbox: batch chmod '%s': %s\n",
                        op->path, strerror(errno));
                ret = 1;
            }
            break;

        case BATCH_TOUCH:
            /*
             * Pass NULL for the times array: POSIX specifies that this sets
             * both atime and mtime to the current time.
             */
            if (utimensat(AT_FDCWD, op->path, NULL, 0) != 0) {
                fprintf(stderr, "matchbox: batch touch '%s': %s\n",
                        op->path, strerror(errno));
                ret = 1;
            }
            break;

        case BATCH_RM_FILE:
            if (unlink(op->path) != 0) {
                fprintf(stderr, "matchbox: batch rm '%s': %s\n",
                        op->path, strerror(errno));
                ret = 1;
            }
            break;

        default:
            fprintf(stderr, "matchbox: unknown batch op type %d for '%s'; skipping\n",
                    (int)op->type, op->path);
            ret = 1;
            break;
        }
    }

    return ret;
}
