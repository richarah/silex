/* intern.h — string intern table: deduplicate immutable strings */
#ifndef MATCHBOX_INTERN_H
#define MATCHBOX_INTERN_H

#include <stddef.h>

/*
 * intern_cstrn: intern the first n bytes of s.
 * Returns a pointer into the intern arena; valid until intern_reset().
 * Never returns NULL: falls back to returning s on OOM.
 * The caller must NOT free the returned pointer.
 */
const char *intern_cstrn(const char *s, size_t n);

/* intern_cstr: intern a NUL-terminated string. */
const char *intern_cstr(const char *s);

/*
 * intern_reset: free all interned strings.
 * All pointers previously returned by intern_cstr/intern_cstrn become invalid.
 * Call between independent top-level scripts; not needed between commands.
 */
void intern_reset(void);

/* Diagnostic counters */
size_t intern_count(void);        /* number of distinct interned strings */
size_t intern_bytes_saved(void);  /* bytes saved vs repeated strdup */

#endif /* MATCHBOX_INTERN_H */
