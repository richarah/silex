/* strbuf.h — bounds-checked, heap-managed string buffer */

#ifndef MATCHBOX_STRBUF_H
#define MATCHBOX_STRBUF_H

#include <stddef.h>

typedef struct {
    char   *buf; /* NUL-terminated content */
    size_t  len; /* current length, not counting NUL */
    size_t  cap; /* allocated capacity including space for NUL */
} strbuf_t;

/* Initialise sb with at least initial_cap bytes of capacity.
 * Returns 0 on success, -1 on allocation failure. */
int sb_init(strbuf_t *sb, size_t initial_cap);

/* Initialise sb from a C string (copies it). */
int sb_init_str(strbuf_t *sb, const char *s);

/* Free the buffer. sb is left in a zero state. */
void sb_free(strbuf_t *sb);

/* Reset length to 0 without freeing; buf[0] = '\0'. */
void sb_reset(strbuf_t *sb);

/* Append a NUL-terminated string. Returns 0 or -1. */
int sb_append(strbuf_t *sb, const char *s);

/* Append exactly n bytes. Returns 0 or -1. */
int sb_appendn(strbuf_t *sb, const char *s, size_t n);

/* Append a single character. Returns 0 or -1. */
int sb_appendc(strbuf_t *sb, char c);

/* Append formatted output (like sprintf). Returns 0 or -1. */
int sb_appendf(strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Return the NUL-terminated C string (never NULL after sb_init). */
static inline const char *sb_str(const strbuf_t *sb) { return sb->buf; }

/* Return current length (not counting NUL). */
static inline size_t sb_len(const strbuf_t *sb) { return sb->len; }

#endif /* MATCHBOX_STRBUF_H */
