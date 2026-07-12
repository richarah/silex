/* classify.c — pattern classification for engine selection */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <string.h>

/*
 * Classify a BRE/ERE pattern into one of the fast-path categories.
 * This runs at compile time (once per pattern).
 */
mb_class_t mb_classify(const char *pat, int flags)
{
    int ere  = (flags & SX_REG_ERE) != 0;
    int icase = (flags & SX_REG_ICASE) != 0;
    (void)icase;

    if (!pat || *pat == '\0')
        return SX_CLASS_SIMPLE;  /* empty pattern: always matches */

    /* Scan for metacharacters */
    int has_meta   = 0;
    int has_anchor_bol = 0;
    int has_anchor_eol = 0;
    int has_backref = 0;
    int has_charclass = 0;

    const char *p = pat;
    size_t len = strlen(pat);

    /* Check for ^ at start */
    if (*p == '^') {
        has_anchor_bol = 1;
        p++;
    }

    /* Check for $ at end (before any trailing metachar) */
    if (len > 0 && pat[len - 1] == '$') {
        /* Make sure $ is not escaped */
        int escaped = 0;
        if (len >= 2 && pat[len - 2] == '\\')
            escaped = 1;
        if (!escaped)
            has_anchor_eol = 1;
    }

    /* Scan body for metacharacters */
    const char *body_start = p;
    while (*p) {
        unsigned char c = (unsigned char)*p;

        if (c == '\\' && p[1] != '\0') {
            char next = p[1];
            if (!ere) {
                /* BRE: \( \) \{ \} \| are metachar; \1-\9 are backrefs */
                if (next >= '1' && next <= '9') {
                    has_backref = 1;
                } else if (next == '(' || next == ')' || next == '{' ||
                           next == '}' || next == '|' || next == '+' ||
                           next == '?') {
                    has_meta = 1;
                }
            } else {
                /* ERE: \1-\9 are backrefs */
                if (next >= '1' && next <= '9')
                    has_backref = 1;
            }
            p += 2;
            continue;
        }

        if (ere) {
            /* ERE metacharacters */
            if (c == '.' || c == '*' || c == '+' || c == '?' ||
                c == '|' || c == '{' || c == '}') {
                has_meta = 1;
            } else if (c == '(' || c == ')') {
                has_meta = 1;
            } else if (c == '[') {
                has_meta = 1;
                has_charclass = 1;
            }
        } else {
            /* BRE metacharacters */
            if (c == '.' || c == '*') {
                has_meta = 1;
            } else if (c == '[') {
                has_meta = 1;
                has_charclass = 1;
            }
        }

        /* $ in middle of pattern is a metachar */
        if (c == '$' && p[1] != '\0') {
            has_meta = 1;
        }

        p++;
    }
    (void)body_start;

    /* Classify */
    if (has_backref)
        return SX_CLASS_BACKREF;

    if (!has_meta) {
        /* No metacharacters (just literal chars, possibly with anchors) */
        if (has_anchor_bol && has_anchor_eol)
            return SX_CLASS_ANCHORED;
        if (has_anchor_bol)
            return SX_CLASS_PREFIX;
        if (!has_anchor_eol)
            return SX_CLASS_FIXED;
        /* has trailing $ but no ^ (e.g. "abc$"): fall through to Thompson NFA */
    }

    /* Has metacharacters but no backrefs */
    if (!has_meta && has_charclass)
        return SX_CLASS_CHARCLASS;

    return SX_CLASS_SIMPLE;
}

/*
 * Extract fixed string from a classified pattern.
 * For SX_CLASS_FIXED: returns the literal string.
 * For SX_CLASS_PREFIX: returns string after '^'.
 * For SX_CLASS_ANCHORED: returns string between '^' and '$'.
 * Result stored in buf (buf_size bytes), NUL-terminated.
 * Returns length, or -1 on error.
 */
int mb_extract_fixed(const char *pat, mb_class_t class,
                     char *buf, size_t buf_size)
{
    const char *src = pat;
    size_t len = strlen(pat);

    /* Skip leading ^ */
    if (class == SX_CLASS_PREFIX || class == SX_CLASS_ANCHORED) {
        if (*src == '^') src++;
    }

    /* Strip trailing $ */
    size_t src_len = strlen(src);
    if (class == SX_CLASS_ANCHORED && src_len > 0 && src[src_len - 1] == '$')
        src_len--;

    if (src_len >= buf_size)
        return -1;

    /* Unescape: BRE/ERE don't have escape sequences for plain literals
     * except \\, \/, etc. Just copy literally for now. */
    size_t out = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            /* \x → x (plain escape) */
            buf[out++] = src[i + 1];
            i++;
        } else {
            buf[out++] = src[i];
        }
    }
    buf[out] = '\0';
    (void)len;
    return (int)out;
}
