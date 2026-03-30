/* regex.h — matchbox Thompson NFA/DFA regex engine public API
 *
 * Drop-in replacement for POSIX regcomp()/regexec() in grep and sed.
 * Provides O(n*m) worst-case Thompson NFA simulation with a lazy DFA cache
 * to amortise repeated state computation. For fixed strings, uses
 * Boyer-Moore-Horspool (O(n/m) average). For patterns with back-references,
 * falls back to POSIX regexec().
 */
#ifndef MATCHBOX_REGEX_H
#define MATCHBOX_REGEX_H

#include <stddef.h>
#include <regex.h>  /* for regex_t fallback */

/* ---- Public types -------------------------------------------------------- */

typedef struct mb_regex mb_regex;

typedef struct {
    const char *start;  /* pointer to first matched byte */
    const char *end;    /* pointer one past last matched byte */
} mb_match;

/* Compilation flags */
enum mb_regex_flags {
    MB_REG_BRE     = 0,        /* POSIX BRE (default) */
    MB_REG_ERE     = 1 << 0,   /* POSIX ERE (-E) */
    MB_REG_ICASE   = 1 << 1,   /* case-insensitive */
    MB_REG_NEWLINE = 1 << 2,   /* ^ and $ match embedded newlines */
    MB_REG_NOSUB   = 1 << 3,   /* no submatch extraction needed */
};

/* ---- Public API ---------------------------------------------------------- */

/*
 * Compile pattern into an mb_regex object.
 * flags: combination of mb_regex_flags.
 * errstr: on error, set to a static error string (do not free).
 * Returns NULL on error.
 */
mb_regex *mb_regex_compile(const char *pat, int flags, const char **errstr);

/*
 * Test whether re matches anywhere in s[0..n-1].
 * If m is non-NULL and a match is found, fills m->start and m->end.
 * Returns 1 if matched, 0 if not.
 */
int mb_regex_search(const mb_regex *re, const char *s, size_t n, mb_match *m);

/*
 * Test whether re matches the string s[0..n-1] (full match from position 0).
 * If m is non-NULL and a match is found, fills m->start and m->end.
 * Returns 1 if matched, 0 if not.
 */
int mb_regex_match(const mb_regex *re, const char *s, size_t n, mb_match *m);

/*
 * Free all memory associated with re.
 */
void mb_regex_free(mb_regex *re);

#endif /* MATCHBOX_REGEX_H */
