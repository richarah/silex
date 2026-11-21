/* regex.h — silex regex engine public API
 *
 * GNU grep compatible BRE/ERE (C locale), including the GNU extensions
 * \< \> \b \B \w \W \s \S \` \'. Patterns are parsed by silex and run on a
 * lazily built DFA (O(n) in the text, no backtracking); a literal prefilter
 * skips text that cannot match. Only patterns with back-references, which
 * no automaton can match, are handed to the C library's regexec().
 *
 * A compiled mb_regex can hold several patterns (grep -e/-f): it matches
 * where any of them matches.
 */
#ifndef SILEX_REGEX_H
#define SILEX_REGEX_H

#include <stddef.h>

/* ---- Public types -------------------------------------------------------- */

typedef struct mb_regex mb_regex;

typedef struct {
    const char *start;  /* pointer to first matched byte */
    const char *end;    /* pointer one past last matched byte */
} mb_match;

/* Compilation flags */
enum mb_regex_flags {
    SX_REG_BRE     = 0,        /* POSIX BRE (default) */
    SX_REG_ERE     = 1 << 0,   /* POSIX ERE (-E) */
    SX_REG_ICASE   = 1 << 1,   /* case-insensitive */
    SX_REG_NEWLINE = 1 << 2,   /* accepted for compatibility; no effect */
    SX_REG_NOSUB   = 1 << 3,   /* accepted for compatibility; no effect */
    SX_REG_FIXED   = 1 << 4,   /* patterns are literal strings (grep -F) */
    SX_REG_WORD    = 1 << 5,   /* match whole words only (grep -w) */
    SX_REG_LINE    = 1 << 6,   /* match whole lines only (grep -x) */
};

/* ---- Single pattern API -------------------------------------------------- */

/*
 * Compile pattern into an mb_regex object.
 * errstr: on error, set to a static error string (do not free).
 * Returns NULL on error.
 */
mb_regex *mb_regex_compile(const char *pat, int flags, const char **errstr);

/*
 * Test whether re matches anywhere in s[0..n-1].
 * If m is non-NULL and a match is found, fills m with the leftmost-longest
 * match. Returns 1 if matched, 0 if not.
 */
int mb_regex_search(const mb_regex *re, const char *s, size_t n, mb_match *m);

/*
 * Like mb_regex_search, but search s[from..n-1] of a longer line s[0..n-1].
 * ^ matches only at s[0], and \b \< \> see the byte before `from`.
 */
int mb_regex_search_from(const mb_regex *re, const char *s, size_t n,
                         size_t from, mb_match *m);

/* Does re match all of s[0..n-1]? */
int mb_regex_match(const mb_regex *re, const char *s, size_t n, mb_match *m);

void mb_regex_free(mb_regex *re);

/*
 * A byte every match contains (0 if there is no single such byte).
 */
unsigned char mb_regex_first_char(const mb_regex *re);

/* ---- Pattern sets (grep) ------------------------------------------------- */

/*
 * Compile npats patterns as one alternation. A pattern containing '\n' is
 * several patterns, as in GNU grep. On error returns NULL, sets *errstr
 * and *errpat (index of the offending pattern). warn, if non-NULL, gets
 * GNU grep's warnings ("* at start of expression"), only when the whole
 * set compiled.
 */
mb_regex *mb_regex_compile_set(const char *const *pats, int npats, int flags,
                               const char **errstr, int *errpat,
                               void (*warn)(const char *msg));

/* Does the line s[0..n) (no '\n') match? Honours SX_REG_WORD/LINE. */
int mb_regex_line(mb_regex *re, const char *s, size_t n);

/*
 * Find the first matching line in buf[0..n): whole lines, each ended by
 * '\n' except possibly the last. Returns 1 and the line as [*ls, *le)
 * (without its '\n'), or 0 when no line matches.
 */
int mb_regex_scan(mb_regex *re, const char *buf, size_t n,
                  size_t *ls, size_t *le);

/*
 * Next match in the line s[0..n) starting at or after `from`, the way GNU
 * grep -o / --color find it: leftmost, then longest; with SX_REG_WORD the
 * first one whose ends are both word boundaries (trying shorter matches at
 * the same start before later starts). May be empty. Returns 1 and the
 * match as [*ms, *me).
 */
int mb_regex_next(mb_regex *re, const char *s, size_t n, size_t from,
                  size_t *ms, size_t *me);

/* Does re match every line (e.g. the empty pattern, or x*)? */
int mb_regex_all_lines(mb_regex *re);

/* Count '\n' in p[0..n) */
size_t mb_regex_count_nl(const char *p, size_t n);

#endif /* SILEX_REGEX_H */
