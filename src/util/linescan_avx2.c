/* linescan_avx2.c — AVX2 vectorised newline scanner (x86_64)
 *
 * Compiled only on x86_64 with -mavx2.  Processes 32 bytes per iteration.
 * Falls back to scalar tail for the remaining bytes.
 *
 * This function intentionally has the same signature as scan_newline() in
 * linescan_scalar.c; the Makefile selects exactly one implementation to link.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "linescan.h"
#include "section.h"

#include <immintrin.h>   /* AVX2 intrinsics */
#include <stdint.h>
#include <string.h>

HOT const char *scan_newline(const char *buf, size_t len)
{
    if (len == 0)
        return buf;

    const char *p   = buf;
    const char *end = buf + len;

    /* Process 32-byte chunks with AVX2 */
    if (len >= 32) {
        __m256i nl = _mm256_set1_epi8('\n');

        while (p + 32 <= end) {
            __m256i v    = _mm256_loadu_si256((const __m256i *)p);
            __m256i cmp  = _mm256_cmpeq_epi8(v, nl);
            int     mask = _mm256_movemask_epi8(cmp);
            if (mask) {
                /* __builtin_ctz gives index of lowest set bit */
                return p + __builtin_ctz((unsigned)mask);
            }
            p += 32;
        }
    }

    /* Scalar tail */
    while (p < end) {
        if (*p == '\n')
            return p;
        p++;
    }
    return end;
}
