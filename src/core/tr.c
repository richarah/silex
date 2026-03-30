/* tr.c -- tr builtin: translate or delete characters */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* tr.c — tr builtin */

#include "../util/error.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Set representation                                                   */
/* ------------------------------------------------------------------ */

#define SET_SIZE 256

/*
 * A character set: flags[c] is 1 if byte c is a member.
 * seq[] holds the ordered expansion used for translation mapping.
 */
typedef struct {
    unsigned char flags[SET_SIZE];
    unsigned char seq[SET_SIZE];
    int           nseq;
} charset_t;

/* ------------------------------------------------------------------ */
/* POSIX class helpers                                                   */
/* ------------------------------------------------------------------ */

typedef int (*class_fn)(int);

static const struct {
    const char *name;
    class_fn    fn;
} posix_classes[] = {
    { "alpha",  isalpha  },
    { "digit",  isdigit  },
    { "alnum",  isalnum  },
    { "upper",  isupper  },
    { "lower",  islower  },
    { "space",  isspace  },
    { "blank",  isblank  },
    { "print",  isprint  },
    { "punct",  ispunct  },
    { "cntrl",  iscntrl  },
    { "graph",  isgraph  },
    { "xdigit", isxdigit },
    { NULL, NULL }
};

/*
 * Try to expand a POSIX class starting at *p, where *p points to ':'
 * (i.e. we have already consumed '[').
 * Appends matching bytes to cs.
 * On success, advances *p past the closing ']' of ':]'.
 * Returns 0 on success, -1 on unrecognised class.
 */
static int expand_posix_class(const char **p, charset_t *cs)
{
    const char *start = *p + 1; /* skip ':' */
    const char *end   = strstr(start, ":]");
    if (!end) return -1;

    size_t len = (size_t)(end - start);
    char name[32];
    if (len == 0 || len >= sizeof(name)) return -1;
    memcpy(name, start, len);
    name[len] = '\0';

    for (int k = 0; posix_classes[k].name; k++) {
        if (strcmp(name, posix_classes[k].name) == 0) {
            for (int c = 0; c < SET_SIZE; c++) {
                if (posix_classes[k].fn(c)) {
                    if (!cs->flags[c]) {
                        cs->flags[c] = 1;
                        if (cs->nseq < SET_SIZE)
                            cs->seq[cs->nseq++] = (unsigned char)c;
                    }
                }
            }
            /* Advance past ':]' then past the closing ']' already consumed */
            *p = end + 2; /* points one past ']' */
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Escape parsing                                                        */
/* ------------------------------------------------------------------ */

/*
 * Parse an octal escape at *p (p points at first octal digit).
 * Advances *p past up to 3 consumed digits.
 */
static unsigned char parse_octal_tr(const char **p)
{
    unsigned val = 0;
    int d = 0;
    while (d < 3 && **p >= '0' && **p <= '7') {
        val = val * 8 + (unsigned)(**p - '0');
        (*p)++;
        d++;
    }
    return (unsigned char)val;
}

/*
 * Parse a backslash escape at *p (p points at char after backslash).
 * Advances *p past the consumed char(s).
 */
static unsigned char parse_escape_tr(const char **p)
{
    unsigned char c;
    switch (**p) {
    case 'a':  c = '\a'; (*p)++; break;
    case 'b':  c = '\b'; (*p)++; break;
    case 'f':  c = '\f'; (*p)++; break;
    case 'n':  c = '\n'; (*p)++; break;
    case 'r':  c = '\r'; (*p)++; break;
    case 't':  c = '\t'; (*p)++; break;
    case 'v':  c = '\v'; (*p)++; break;
    case '\\': c = '\\'; (*p)++; break;
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
        c = parse_octal_tr(p);
        break;
    default:
        c = (unsigned char)**p;
        (*p)++;
        break;
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* Set parsing                                                           */
/* ------------------------------------------------------------------ */

/*
 * Parse a set specification string into cs.
 * Handles: literals, a-z ranges, [:class:], \NNN octal escapes.
 * Returns 0 on success, -1 on error.
 */
static int parse_set(const char *str, charset_t *cs)
{
    memset(cs, 0, sizeof(*cs));
    const char *p = str;

    while (*p) {
        /* POSIX class [: ... :] */
        if (p[0] == '[' && p[1] == ':') {
            p++; /* now points at ':' */
            if (expand_posix_class(&p, cs) == 0)
                continue;
            /* If unrecognised, treat '[' literally and retry from ':' */
            p--; /* back to '[' */
            unsigned char c = (unsigned char)'[';
            if (!cs->flags[c]) {
                cs->flags[c] = 1;
                if (cs->nseq < SET_SIZE) cs->seq[cs->nseq++] = c;
            }
            p++;
            continue;
        }

        /* Backslash escape */
        if (*p == '\\') {
            p++;
            if (*p == '\0') break;
            unsigned char c = parse_escape_tr(&p);
            if (!cs->flags[c]) {
                cs->flags[c] = 1;
                if (cs->nseq < SET_SIZE) cs->seq[cs->nseq++] = c;
            }
            continue;
        }

        /* Range: lo - hi  (but '-' at start or end is literal) */
        if (p[1] == '-' && p[2] != '\0') {
            unsigned char lo = (unsigned char)p[0];
            p += 2; /* skip 'lo' and '-' */
            unsigned char hi;
            if (*p == '\\') {
                p++;
                hi = parse_escape_tr(&p);
            } else {
                hi = (unsigned char)*p;
                p++;
            }
            if (lo > hi) {
                err_msg("tr", "range '\\%o-\\%o' is out of order",
                        (unsigned)lo, (unsigned)hi);
                return -1;
            }
            for (unsigned int c = (unsigned int)lo; c <= (unsigned int)hi; c++) {
                if (!cs->flags[c]) {
                    cs->flags[c] = 1;
                    if (cs->nseq < SET_SIZE)
                        cs->seq[cs->nseq++] = (unsigned char)c;
                }
            }
            continue;
        }

        /* Literal character */
        unsigned char c = (unsigned char)*p++;
        if (!cs->flags[c]) {
            cs->flags[c] = 1;
            if (cs->nseq < SET_SIZE) cs->seq[cs->nseq++] = c;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Complement                                                            */
/* ------------------------------------------------------------------ */

/*
 * Build the complement of cs (all bytes NOT in cs), in byte order 0..255.
 */
static void complement_set(const charset_t *cs, charset_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int c = 0; c < SET_SIZE; c++) {
        if (!cs->flags[c]) {
            out->flags[c] = 1;
            if (out->nseq < SET_SIZE)
                out->seq[out->nseq++] = (unsigned char)c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Translation table                                                    */
/* ------------------------------------------------------------------ */

/*
 * Build a 256-byte translation table mapping set1 -> set2.
 * If set2 is shorter than set1, the last byte of set2 is reused.
 */
static void build_xlat(const charset_t *s1, const charset_t *s2,
                        unsigned char xlat[SET_SIZE])
{
    for (int i = 0; i < SET_SIZE; i++)
        xlat[i] = (unsigned char)i;

    int n2 = s2->nseq;
    if (n2 == 0) return;

    for (int i = 0; i < s1->nseq; i++) {
        unsigned char from = s1->seq[i];
        unsigned char to   = (i < n2) ? s2->seq[i] : s2->seq[n2 - 1];
        xlat[from] = to;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */

int applet_tr(int argc, char **argv)
{
    int opt_d = 0; /* -d: delete */
    int opt_s = 0; /* -s: squeeze */
    int opt_c = 0; /* -c/-C: complement */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'd': opt_d = 1; break;
            case 's': opt_s = 1; break;
            case 'c': case 'C': opt_c = 1; break;
            default:
                err_msg("tr", "unrecognized option '-%c'", *p);
                err_usage("tr", "[-dsc] SET1 [SET2]");
                return 1;
            }
            p++;
        }
    }

    int nsets = argc - i;

    /* Validate argument counts */
    if (opt_d) {
        /* -d requires SET1; -d -s may optionally take SET2 for squeeze */
        if (nsets < 1) {
            err_usage("tr", "-d [-s] SET1 [SET2]");
            return 1;
        }
    } else if (opt_s && nsets == 1) {
        /* -s without -d: squeeze SET1 chars */
        /* allowed */
    } else if (nsets < 2) {
        err_usage("tr", "[-cs] SET1 SET2");
        return 1;
    }

    charset_t set1_raw, set1;
    charset_t set2;
    memset(&set2, 0, sizeof(set2));

    if (parse_set(argv[i], &set1_raw) != 0) return 1;

    if (opt_c)
        complement_set(&set1_raw, &set1);
    else
        set1 = set1_raw;

    if (nsets >= 2) {
        if (parse_set(argv[i + 1], &set2) != 0) return 1;
    }

    /* Build translation table (used when not in pure delete mode) */
    unsigned char xlat[SET_SIZE];
    if (!opt_d) {
        if (nsets >= 2)
            build_xlat(&set1, &set2, xlat);
        else {
            /* -s only: identity translation */
            for (int k = 0; k < SET_SIZE; k++)
                xlat[k] = (unsigned char)k;
        }
    }

    /* Process stdin -> stdout */
    int c;
    int last_out = -1; /* last byte written, for squeeze */

    while ((c = getchar()) != EOF) {
        unsigned char uc = (unsigned char)c;

        if (opt_d) {
            /* Delete chars in set1 */
            if (set1.flags[uc]) continue;

            /* -d -s SET2: additionally squeeze consecutive set2 chars */
            if (opt_s && nsets >= 2 && set2.flags[uc]) {
                if (last_out >= 0 && uc == (unsigned char)last_out) continue;
            }

            putchar(uc);
            last_out = uc;
            continue;
        }

        /* Translate */
        unsigned char out = xlat[uc];

        /* -s: squeeze consecutive identical translated chars that are in set2 */
        if (opt_s) {
            int in_set2 = (nsets >= 2) ? set2.flags[out] : set1.flags[uc];
            if (in_set2 && last_out >= 0 && out == (unsigned char)last_out)
                continue;
        }

        putchar(out);
        last_out = out;
    }

    return 0;
}
