/* linescan_neon.c — NEON vectorised newline scanner (aarch64) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "linescan.h"

#include <arm_neon.h>
#include <string.h>

const char *scan_newline(const char *buf, size_t len)
{
    if (len == 0)
        return buf;

    const char *p   = buf;
    const char *end = buf + len;

    if (len >= 16) {
        uint8x16_t nl = vdupq_n_u8('\n');

        while (p + 16 <= end) {
            uint8x16_t v   = vld1q_u8((const uint8_t *)p);
            uint8x16_t cmp = vceqq_u8(v, nl);
            /* Collapse to 64-bit: if any lane matched, value is nonzero */
            uint64_t   lo  = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
            uint64_t   hi  = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
            if (lo | hi) {
                /* Fall back to byte scan within this chunk */
                for (int i = 0; i < 16; i++) {
                    if (p[i] == '\n')
                        return p + i;
                }
            }
            p += 16;
        }
    }

    while (p < end) {
        if (*p == '\n')
            return p;
        p++;
    }
    return end;
}
