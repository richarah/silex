/* charclass_re.c — bracket expressions -> 256-bit byte sets
 *
 * Follows glibc's parse_bracket_exp() (which GNU grep runs on every pattern
 * to validate it) for what is an error and what each error is called, and
 * GNU dfa.c for the one extra error it adds ("[:space:]" without the outer
 * brackets). C locale: one byte is one character, collating elements and
 * equivalence classes are single characters, ranges are byte ranges.
 * A backslash is an ordinary character inside brackets.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <string.h>

#define E_BADPAT   "Invalid regular expression"
#define E_EBRACK   "Unmatched [, [^, [:, [., or [="
#define E_ERANGE   "Invalid range end"
#define E_ECTYPE   "Invalid character class name"
#define E_ECOLLATE "Invalid collation character"
#define E_COLON    "character class syntax is [[:space:]], not [:space:]"

#define NAME_MAX_ 32   /* glibc BRACKET_NAME_BUF_SIZE */

enum { EL_CHAR, EL_COLL, EL_EQUIV, EL_CLASS };

typedef struct {
    int  type;
    unsigned char ch;
    char name[NAME_MAX_ + 1];
    size_t nlen;
} elem_t;

static int class_set(mb_charclass *cc, const char *name, int icase)
{
    static const char *const names[] = {
        "alpha", "upper", "lower", "digit", "xdigit", "space",
        "print", "punct", "graph", "cntrl", "blank", "alnum", NULL
    };
    int k;
    for (k = 0; names[k]; k++)
        if (strcmp(name, names[k]) == 0) break;
    if (!names[k]) return -1;
    /* dfa.c: with -i, [:upper:] and [:lower:] both mean [:alpha:] */
    if (icase && (k == 1 || k == 2)) k = 0;
    for (int c = 0; c < 256; c++) {
        int m;
        switch (k) {
        case 0:  m = isalpha(c);  break;
        case 1:  m = isupper(c);  break;
        case 2:  m = islower(c);  break;
        case 3:  m = isdigit(c);  break;
        case 4:  m = isxdigit(c); break;
        case 5:  m = isspace(c);  break;
        case 6:  m = isprint(c);  break;
        case 7:  m = ispunct(c);  break;
        case 8:  m = isgraph(c);  break;
        case 9:  m = iscntrl(c);  break;
        case 10: m = (c == ' ' || c == '\t'); break;
        default: m = isalnum(c);  break;
        }
        if (m) mb_charclass_set(cc, (unsigned char)c);
    }
    return 0;
}

/*
 * Parse one element at *pp. accept_hyphen: a '-' may stand here (first
 * element, or the end of a range).
 */
static int parse_elem(const char **pp, const char *end, int accept_hyphen,
                      elem_t *el, const char **err)
{
    const char *p = *pp;
    if (p >= end) { *err = E_EBRACK; return -1; }
    if (*p == '[' && p + 1 < end &&
        (p[1] == '.' || p[1] == '=' || p[1] == ':')) {
        char delim = p[1];
        p += 2;
        if (p >= end) { *err = E_EBRACK; return -1; }
        size_t i = 0;
        for (;; i++) {
            if (i >= NAME_MAX_) { *err = E_EBRACK; return -1; }
            if (p >= end) { *err = E_EBRACK; return -1; }
            char ch = *p++;
            if (p >= end) { *err = E_EBRACK; return -1; }
            if (ch == delim && *p == ']') break;
            el->name[i] = ch;
        }
        p++;   /* the ']' */
        el->name[i] = '\0';
        el->nlen = i;
        el->type = delim == '.' ? EL_COLL : delim == '=' ? EL_EQUIV : EL_CLASS;
        *pp = p;
        return 0;
    }
    if (*p == '-' && !accept_hyphen) {
        /* A '-' that does not start a range must be the last character. */
        if (!(p + 1 < end && p[1] == ']')) { *err = E_ERANGE; return -1; }
    }
    el->type = EL_CHAR;
    el->ch = (unsigned char)*p;
    *pp = p + 1;
    return 0;
}

/* The single character an element names, for a range endpoint. */
static int elem_char(const elem_t *el, unsigned char *c, const char **err)
{
    if (el->type == EL_CHAR) { *c = el->ch; return 0; }
    if (el->type == EL_COLL) {
        if (el->nlen != 1) { *err = E_ECOLLATE; return -1; }
        *c = (unsigned char)el->name[0];
        return 0;
    }
    *err = E_ERANGE;
    return -1;
}

int mb_bracket_parse(const char *p, const char *end, int icase,
                     mb_charclass *out, const char **endp, const char **err)
{
    memset(out, 0, sizeof(*out));
    int invert = 0;

    if (p >= end) { *err = E_BADPAT; return -1; }
    if (*p == '^') {
        invert = 1;
        p++;
        if (p >= end) { *err = E_BADPAT; return -1; }
    }

    /* dfa.c's "[:space:]" check: 1 = starts with ':', 2 = last element
     * is ':', 4 = some other element, 8 = a class or range (no warning) */
    int colon = (*p == ':');
    int first = 1;

    for (;;) {
        elem_t a;
        memset(&a, 0, sizeof(a));
        if (parse_elem(&p, end, first, &a, err) < 0) return -1;
        first = 0;

        int is_range = 0;
        elem_t b;
        memset(&b, 0, sizeof(b));
        if (a.type != EL_CLASS && a.type != EL_EQUIV) {
            if (p >= end) { *err = E_EBRACK; return -1; }
            if (*p == '-') {
                const char *q = p + 1;
                if (q >= end) { *err = E_EBRACK; return -1; }
                if (*q != ']') {
                    p = q;
                    if (parse_elem(&p, end, 1, &b, err) < 0) return -1;
                    is_range = 1;
                }
                /* else: "-]" -- the '-' is a literal, read next round */
            }
        }

        colon &= ~2;
        if (is_range) {
            unsigned char lo, hi;
            if (elem_char(&a, &lo, err) < 0) return -1;
            if (elem_char(&b, &hi, err) < 0) return -1;
            if (lo > hi) { *err = E_ERANGE; return -1; }
            for (unsigned c = lo; c <= hi; c++)
                mb_charclass_set(out, (unsigned char)c);
            colon |= 8;
        } else {
            switch (a.type) {
            case EL_CHAR:
                mb_charclass_set(out, a.ch);
                colon |= (a.ch == ':') ? 2 : 4;
                break;
            case EL_COLL:
            case EL_EQUIV:
                if (a.nlen != 1) { *err = E_ECOLLATE; return -1; }
                mb_charclass_set(out, (unsigned char)a.name[0]);
                colon |= 8;
                break;
            case EL_CLASS:
                if (class_set(out, a.name, icase) < 0) { *err = E_ECTYPE; return -1; }
                colon |= 8;
                break;
            }
        }

        if (p >= end) { *err = E_EBRACK; return -1; }
        if (*p == ']') { p++; break; }
    }

    if (icase) {
        mb_charclass tmp = *out;
        for (int c = 0; c < 256; c++) {
            if (mb_charclass_test(&tmp, (unsigned char)c)) {
                mb_charclass_set(out, (unsigned char)tolower(c));
                mb_charclass_set(out, (unsigned char)toupper(c));
            }
        }
    }
    if (invert)
        for (int i = 0; i < 32; i++) out->bits[i] ^= 0xFF;

    *endp = p;
    if (colon == 7) { *err = E_COLON; return 1; }   /* valid, but dfa.c rejects */
    return 0;
}
