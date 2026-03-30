/* detect.c — io_uring and inotify capability detection */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * detect.c — independence detection for batch operations.
 *
 * Two operations are considered NOT independent if:
 *   - Any two paths share a prefix where one path is a strict prefix of
 *     another (i.e. one path is an ancestor directory of the other).
 *   - Any two operations touch the same path.
 *
 * We are conservative: if there is any doubt, we return 0 (sequential).
 *
 * Algorithm:
 *   For each ordered pair (A, B) of operations, check:
 *     1. path_A == path_B  (same path -> not independent)
 *     2. path_A is a proper prefix of path_B followed by '/'
 *        (A is an ancestor of B -> not independent)
 *     3. path_B is a proper prefix of path_A followed by '/'
 *        (B is an ancestor of A -> not independent)
 *
 * O(n^2) in the number of operations; acceptable for the small batches
 * expected in a container build runtime.
 */

#include "detect.h"

#include <string.h>

/*
 * Returns 1 if path `a` is a proper prefix of path `b`,
 * i.e. b starts with a followed by '/'.
 * Does NOT consider a == b as a prefix relationship.
 */
static int is_path_prefix(const char *a, const char *b)
{
    size_t alen = strlen(a);
    if (alen == 0) {
        /* Empty string is a prefix of everything — conservative */
        return 1;
    }
    if (strncmp(a, b, alen) != 0)
        return 0;
    /* b must have a '/' at position alen, or alen == strlen(b) means equal */
    if (b[alen] == '/' || b[alen] == '\0')
        return 1;
    return 0;
}

int batch_ops_independent(batch_op_t *ops)
{
    if (!ops || !ops->next)
        return 1; /* 0 or 1 operation is trivially independent */

    /* Count operations and store pointers for O(n^2) comparison */
#define MAX_OPS 1024
    const char *paths[MAX_OPS];
    int n = 0;

    for (batch_op_t *op = ops; op && n < MAX_OPS; op = op->next) {
        if (!op->path)
            return 0; /* NULL path is undefined behaviour; be conservative */
        paths[n++] = op->path;
    }

    if (ops->next && n == MAX_OPS) {
        /* Too many operations to check conservatively: treat as dependent */
        return 0;
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            const char *a = paths[i];
            const char *b = paths[j];

            /* Same path */
            if (strcmp(a, b) == 0)
                return 0;

            /* One is a directory ancestor of the other */
            if (is_path_prefix(a, b))
                return 0;
            if (is_path_prefix(b, a))
                return 0;
        }
    }

    return 1;
#undef MAX_OPS
}
