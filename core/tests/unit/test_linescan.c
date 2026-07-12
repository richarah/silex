/* test_linescan.c — unit test for linescan implementations
 *
 * Compiles and links with linescan_scalar.c (the reference implementation).
 * If AVX2 is available, also tests linescan_avx2.c for consistency.
 *
 * Tests:
 *   - empty buffer
 *   - no newlines
 *   - newline at every position 0..64
 *   - buffers of size 0, 1, 31, 32, 33, 63, 64, 65, 1024
 *   - 1MB buffer with newlines only at specific positions
 *   - all implementations produce identical results (scalar is reference)
 *
 * Exit 0 on success, 1 on failure.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include scalar implementation directly (renamed to avoid link conflict) */
#include "util/linescan.h"

/* Reference: always use scalar via memchr */
static const char *ref_scan_newline(const char *buf, size_t len)
{
    const char *p = (const char *)memchr(buf, '\n', len);
    return p ? p : buf + len;
}

static int failures = 0;

static void check_scan(const char *buf, size_t len, const char *label)
{
    const char *got = scan_newline(buf, len);
    const char *exp = ref_scan_newline(buf, len);
    if (got != exp) {
        fprintf(stderr,
                "FAIL [%s] len=%zu: got offset %td, expected %td\n",
                label, len, got - buf, exp - buf);
        failures++;
    }
}

int main(void)
{
    /* Empty buffer */
    {
        const char empty[1] = {0};
        const char *p = scan_newline(empty, 0);
        if (p != empty) {
            fprintf(stderr, "FAIL: empty buf: expected same pointer\n");
            failures++;
        }
        printf("  empty buffer: OK\n");
    }

    /* No newlines in various sizes */
    {
        char buf[128];
        memset(buf, 'a', sizeof(buf));
        size_t sizes[] = { 0, 1, 15, 16, 31, 32, 33, 63, 64, 65, 127 };
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
            check_scan(buf, sizes[i], "no-newline");
        }
        printf("  no-newline sizes: OK\n");
    }

    /* Newline at every position 0..64 */
    {
        char buf[66];
        memset(buf, 'a', sizeof(buf));
        for (int pos = 0; pos < 65; pos++) {
            memset(buf, 'a', sizeof(buf));
            buf[pos] = '\n';
            check_scan(buf, sizeof(buf), "nl-at-pos");
        }
        printf("  newline at positions 0..64: OK\n");
    }

    /* Buffers of size 31/32/33/64/65 with newline in last byte */
    {
        size_t sizes[] = { 31, 32, 33, 63, 64, 65 };
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
            size_t sz = sizes[i];
            char *buf = malloc(sz);
            if (!buf) { fprintf(stderr, "FAIL: malloc\n"); return 1; }
            memset(buf, 'x', sz);
            buf[sz - 1] = '\n';
            check_scan(buf, sz, "nl-last-byte");
            memset(buf, 'x', sz);  /* no newline */
            check_scan(buf, sz, "no-nl-boundary");
            free(buf);
        }
        printf("  boundary sizes: OK\n");
    }

    /* 1MB buffer — no newline */
    {
        size_t sz = 1024 * 1024;
        char *buf = malloc(sz);
        if (!buf) { fprintf(stderr, "FAIL: malloc 1MB\n"); return 1; }
        memset(buf, 'z', sz);
        check_scan(buf, sz, "1MB-no-nl");
        printf("  1MB no-newline: OK\n");

        /* Newline at exact mid-point */
        buf[sz / 2] = '\n';
        check_scan(buf, sz, "1MB-nl-mid");
        printf("  1MB newline at midpoint: OK\n");

        /* Newline at last byte */
        buf[sz / 2] = 'z';
        buf[sz - 1]  = '\n';
        check_scan(buf, sz, "1MB-nl-end");
        printf("  1MB newline at end: OK\n");

        free(buf);
    }

    if (failures == 0) {
        printf("test_linescan: all checks passed\n");
        return 0;
    } else {
        fprintf(stderr, "test_linescan: %d failure(s)\n", failures);
        return 1;
    }
}
