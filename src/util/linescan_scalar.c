/* linescan_scalar.c — scalar newline scanner using memchr
 *
 * Always compiled; used as fallback and for testing reference.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "linescan.h"

#include <string.h>

const char *scan_newline(const char *buf, size_t len)
{
    const char *p = (const char *)memchr(buf, '\n', len);
    return p ? p : buf + len;
}
