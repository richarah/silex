/* mb_regex.c — mb_regex public API: compile, search, match, free */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- mb_regex_compile ----------------------------------------------------- */

mb_regex *mb_regex_compile(const char *pat, int flags, const char **errstr)
{
    if (errstr) *errstr = NULL;

    mb_regex *re = calloc(1, sizeof(mb_regex));
    if (!re) {
        if (errstr) *errstr = "out of memory";
        return NULL;
    }
    re->flags = flags;

    /* Classify the pattern */
    re->class = mb_classify(pat, flags);

    switch (re->class) {
    case SX_CLASS_FIXED:
    case SX_CLASS_PREFIX:
    case SX_CLASS_ANCHORED: {
        /* Extract fixed string */
        char buf[512];
        int flen = mb_extract_fixed(pat, re->class, buf, sizeof(buf));
        if (flen < 0) {
            /* Too long for buffer: fall back to SIMPLE */
            re->class = SX_CLASS_SIMPLE;
            goto do_simple;
        }
        re->fixed_len = (size_t)flen;
        memcpy(re->fixed_str, buf, re->fixed_len + 1);

        /* Build BMH skip table only for FIXED class (full-text search).
         * PREFIX and ANCHORED use memcmp/strcmp and don't need BMH. */
        if (re->class == SX_CLASS_FIXED) {
            if (flags & SX_REG_ICASE)
                mb_bmh_build_icase(re->fixed_str, re->fixed_len, re->bmh_skip);
            else
                mb_bmh_build(re->fixed_str, re->fixed_len, re->bmh_skip);
        }
        return re;
    }

    case SX_CLASS_CHARCLASS:
    case SX_CLASS_SIMPLE:
    do_simple: {
        mb_prog_init(&re->prog);
        const char *err = NULL;
        if (mb_parse(pat, flags, &re->prog, &err) < 0) {
            mb_prog_free(&re->prog);
            /* Fall back to POSIX */
            re->class = SX_CLASS_BACKREF;
            goto do_posix;
        }
        return re;
    }

    case SX_CLASS_BACKREF:
    do_posix: {
        /* POSIX regcomp fallback */
        int rflags = 0;
        if (flags & SX_REG_ERE)     rflags |= REG_EXTENDED;
        if (flags & SX_REG_ICASE)   rflags |= REG_ICASE;
        if (flags & SX_REG_NEWLINE) rflags |= REG_NEWLINE;
        if (flags & SX_REG_NOSUB)   rflags |= REG_NOSUB;

        int rc = regcomp(&re->posix_re, pat, rflags);
        if (rc != 0) {
            char errbuf[256];
            regerror(rc, &re->posix_re, errbuf, sizeof(errbuf));
            if (errstr) {
                static char static_err[256];
                snprintf(static_err, sizeof(static_err), "%s", errbuf);
                *errstr = static_err;
            }
            free(re);
            return NULL;
        }
        re->posix_ok = 1;
        return re;
    }
    }

    /* Unreachable */
    free(re);
    return NULL;
}

/* ---- mb_regex_search ------------------------------------------------------ */

int mb_regex_search(const mb_regex *re, const char *s, size_t n, mb_match *m)
{
    if (!re || !s) return 0;

    switch (re->class) {
    case SX_CLASS_FIXED: {
        const char *found;
        if (re->flags & SX_REG_ICASE)
            found = mb_bmh_search_icase(s, n, re->fixed_str, re->fixed_len,
                                         re->bmh_skip);
        else
            found = mb_bmh_search(s, n, re->fixed_str, re->fixed_len,
                                   re->bmh_skip);
        if (!found) return 0;
        if (m) { m->start = found; m->end = found + re->fixed_len; }
        return 1;
    }

    case SX_CLASS_PREFIX: {
        /* Pattern: ^literal — only matches at start of string */
        if (re->fixed_len > n) return 0;
        int match;
        if (re->flags & SX_REG_ICASE) {
            match = (strncasecmp(s, re->fixed_str, re->fixed_len) == 0);
        } else {
            match = (memcmp(s, re->fixed_str, re->fixed_len) == 0);
        }
        if (!match) return 0;
        if (m) { m->start = s; m->end = s + re->fixed_len; }
        return 1;
    }

    case SX_CLASS_ANCHORED: {
        /* Pattern: ^literal$ — must match entire string */
        if (re->fixed_len != n) return 0;
        int match;
        if (re->flags & SX_REG_ICASE) {
            match = (strncasecmp(s, re->fixed_str, n) == 0);
        } else {
            match = (memcmp(s, re->fixed_str, n) == 0);
        }
        if (!match) return 0;
        if (m) { m->start = s; m->end = s + n; }
        return 1;
    }

    case SX_CLASS_CHARCLASS:
    case SX_CLASS_SIMPLE: {
        int anchor_bol = 0;
        /* Check if pattern starts with BOL assertion */
        if (re->prog.len > 0) {
            const mb_instr *first = &re->prog.instrs[0];
            if (first->op == I_BOL) anchor_bol = 1;
            if (first->op == I_JUMP && first->x >= 0 &&
                first->x < re->prog.len &&
                re->prog.instrs[first->x].op == I_BOL)
                anchor_bol = 1;
        }
        return mb_thompson_search(&re->prog, re->flags, s, n, m, anchor_bol);
    }

    case SX_CLASS_BACKREF: {
        if (!re->posix_ok) return 0;
        regmatch_t pm;
        int rc = regexec(&re->posix_re, s, 1, &pm, 0);
        if (rc != 0) return 0;
        if (m) {
            m->start = s + pm.rm_so;
            m->end   = s + pm.rm_eo;
        }
        return 1;
    }
    }
    return 0;
}

/* ---- mb_regex_match ------------------------------------------------------- */

int mb_regex_match(const mb_regex *re, const char *s, size_t n, mb_match *m)
{
    /* Full match: pattern must cover entire string.
     * For most use cases, just wrap in anchors. Simplest approach:
     * do a search and check if the result covers [0, n). */
    mb_match tmp;
    if (!mb_regex_search(re, s, n, &tmp)) return 0;
    if (tmp.start != s || tmp.end != s + n) return 0;
    if (m) *m = tmp;
    return 1;
}

/* ---- mb_regex_first_char -------------------------------------------------- */

unsigned char mb_regex_first_char(const mb_regex *re)
{
    if (!re) return 0;
    switch (re->class) {
    case SX_CLASS_FIXED:
    case SX_CLASS_PREFIX:
    case SX_CLASS_ANCHORED:
        return re->fixed_len > 0 ? (unsigned char)re->fixed_str[0] : 0;
    case SX_CLASS_SIMPLE:
    case SX_CLASS_CHARCLASS:
        if (re->prog.len > 0) {
            const mb_instr *in = &re->prog.instrs[0];
            if (in->op == I_CHAR)
                return (unsigned char)in->arg.c;
            /* Skip a leading BOL assertion if the next state is a literal */
            if (in->op == I_BOL && in->x >= 0 && in->x < re->prog.len) {
                const mb_instr *in2 = &re->prog.instrs[in->x];
                if (in2->op == I_CHAR)
                    return (unsigned char)in2->arg.c;
            }
        }
        return 0;
    default:
        return 0;
    }
}

/* ---- mb_regex_free -------------------------------------------------------- */

void mb_regex_free(mb_regex *re)
{
    if (!re) return;
    if (re->class == SX_CLASS_SIMPLE || re->class == SX_CLASS_CHARCLASS)
        mb_prog_free(&re->prog);
    if (re->posix_ok)
        regfree(&re->posix_re);
    free(re);
}
