/* linescan.h — fast newline/NUL scanning for wc and sort
 *
 * Provides scan_newline(buf, len) that returns a pointer to the first '\n'
 * in buf[0..len-1], or buf+len if not found.
 *
 * The implementation is selected at compile time:
 *   linescan_avx2.c   — x86_64 with AVX2 (32-byte vectors)
 *   linescan_neon.c   — aarch64 with NEON (16-byte vectors)
 *   linescan_scalar.c — generic (wraps memchr)
 *
 * All three implementations must produce identical results; see
 * tests/unit/test_linescan.c for cross-validation.
 */
#ifndef MATCHBOX_LINESCAN_H
#define MATCHBOX_LINESCAN_H

#include <stddef.h>

/*
 * scan_newline — find first '\n' in buf[0..len)
 *
 * Returns a pointer to the first '\n', or buf+len if none found.
 * buf must be a valid pointer; len may be zero (returns buf immediately).
 */
const char *scan_newline(const char *buf, size_t len);

#endif /* MATCHBOX_LINESCAN_H */
