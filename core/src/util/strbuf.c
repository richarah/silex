/* strbuf.c — bounds-checked, heap-managed string buffer */

#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB_MIN_CAP  64
#define SB_MAX_CAP  (64 * 1024 * 1024)  /* 64 MB hard cap — no shell string is ever larger */

static int sb_grow(strbuf_t *sb, size_t needed)
{
    if (needed + 1 > SB_MAX_CAP)
        return -1;  /* requested size exceeds hard cap */

    size_t new_cap = sb->cap ? sb->cap * 2 : SB_MIN_CAP;
    while (new_cap < needed + 1) /* +1 for NUL */
        new_cap *= 2;
    if (new_cap > SB_MAX_CAP)
        new_cap = SB_MAX_CAP;

    char *p = realloc(sb->buf, new_cap);
    if (!p)
        return -1;
    sb->buf = p;
    sb->cap = new_cap;
    return 0;
}

int sb_init(strbuf_t *sb, size_t initial_cap)
{
    size_t cap = initial_cap > SB_MIN_CAP ? initial_cap : SB_MIN_CAP;
    sb->buf = malloc(cap);
    if (!sb->buf)
        return -1;
    sb->buf[0] = '\0';
    sb->len = 0;
    sb->cap = cap;
    return 0;
}

int sb_init_str(strbuf_t *sb, const char *s)
{
    size_t slen = strlen(s);
    if (sb_init(sb, slen + 1) != 0)
        return -1;
    memcpy(sb->buf, s, slen + 1);
    sb->len = slen;
    return 0;
}

void sb_free(strbuf_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_reset(strbuf_t *sb)
{
    sb->len = 0;
    if (sb->buf)
        sb->buf[0] = '\0';
}

int sb_appendn(strbuf_t *sb, const char *s, size_t n)
{
    if (sb->len + n >= sb->cap) {
        if (sb_grow(sb, sb->len + n) != 0)
            return -1;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

int sb_append(strbuf_t *sb, const char *s)
{
    return sb_appendn(sb, s, strlen(s));
}

int sb_appendc(strbuf_t *sb, char c)
{
    return sb_appendn(sb, &c, 1);
}

int sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    int needed;

    /* First pass: measure */
    va_start(ap, fmt);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0)
        return -1;

    if (sb->len + (size_t)needed >= sb->cap) {
        if (sb_grow(sb, sb->len + (size_t)needed) != 0)
            return -1;
    }

    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    sb->len += (size_t)needed;
    return 0;
}
