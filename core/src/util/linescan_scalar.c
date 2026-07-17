/* linescan_scalar.c — SWAR scalar newline scanner (8 bytes/cycle) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "linescan.h"

#include <stdint.h>
#include <string.h>

/*
 * SWAR (SIMD Within A Register): test 8 bytes for '\n' (0x0A)
 * in a single 64-bit operation.
 *
 * XOR with 0x0A..0A to turn '\n' bytes into 0x00, then apply
 * the "has zero byte" test: (x - 0x01..01) & ~x & 0x80..80
 * High bit of each lane is set iff the original byte was '\n'.
 */
static inline uint64_t has_newline_swar(uint64_t x)
{
    uint64_t xored = x ^ UINT64_C(0x0A0A0A0A0A0A0A0A);
    return (xored - UINT64_C(0x0101010101010101))
         & ~xored
         & UINT64_C(0x8080808080808080);
}

const char *scan_newline(const char *buf, size_t len)
{
    const unsigned char *p   = (const unsigned char *)buf;
    const unsigned char *end = p + len;

    /* Align to 8-byte boundary */
    while (p < end && ((uintptr_t)p & 7u)) {
        if (*p == '\n')
            return (const char *)p;
        p++;
    }

    /* SWAR: process 8 bytes at a time */
    const unsigned char *end8 = end >= p + 8 ? end - 7 : p;
    while (p < end8) {
        uint64_t word;
        memcpy(&word, p, 8);   /* aliasing-safe unaligned load */
        if (has_newline_swar(word)) {
            /* '\n' is in this word — find exact byte */
            for (int i = 0; i < 8; i++) {
                if (p[i] == '\n')
                    return (const char *)(p + i);
            }
        }
        p += 8;
    }

    /* Scalar tail (< 8 bytes remaining) */
    while (p < end) {
        if (*p == '\n')
            return (const char *)p;
        p++;
    }

    return (const char *)end;
}
