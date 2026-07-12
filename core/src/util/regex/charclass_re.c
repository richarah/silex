/* charclass_re.c — parse regex character classes into 256-bit bitmaps */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <string.h>

/*
 * Parse a POSIX character class name (without the enclosing [: :]).
 * Sets the corresponding bits in cc. Returns 1 on success, 0 on unknown class.
 */
static int apply_posix_class(mb_charclass *cc, const char *name, int negate)
{
    int found = 0;

    for (int c = 0; c < 256; c++) {
        int match = 0;
        unsigned char uc = (unsigned char)c;

        if (strcmp(name, "alpha")  == 0) match = isalpha(uc);
        else if (strcmp(name, "digit")  == 0) match = isdigit(uc);
        else if (strcmp(name, "alnum")  == 0) match = isalnum(uc);
        else if (strcmp(name, "space")  == 0) match = isspace(uc);
        else if (strcmp(name, "blank")  == 0) match = (uc == ' ' || uc == '\t');
        else if (strcmp(name, "upper")  == 0) match = isupper(uc);
        else if (strcmp(name, "lower")  == 0) match = islower(uc);
        else if (strcmp(name, "print")  == 0) match = isprint(uc);
        else if (strcmp(name, "punct")  == 0) match = ispunct(uc);
        else if (strcmp(name, "graph")  == 0) match = isgraph(uc);
        else if (strcmp(name, "cntrl")  == 0) match = iscntrl(uc);
        else if (strcmp(name, "xdigit") == 0) match = isxdigit(uc);
        else { return 0; }  /* unknown class */

        found = 1;
        if (!negate && match)
            mb_charclass_set(cc, (unsigned char)c);
    }
    return found;
}

/*
 * Parse a bracket expression starting after '[' (pat points to first char
 * after '[', end points to the closing ']' or end of string).
 * Handles: [abc], [a-z], [^...], [[:name:]], combinations.
 * Fills out->bits.
 * Returns 1 on success, 0 on error.
 */
int mb_charclass_parse(const char *pat, const char *end,
                       mb_charclass *out, int flags)
{
    int icase = (flags & SX_REG_ICASE) != 0;
    memset(out->bits, 0, sizeof(out->bits));

    const char *p = pat;
    int negate = 0;

    /* Negation */
    if (*p == '^') {
        negate = 1;
        p++;
    }

    /* ] as first char is literal */
    if (*p == ']') {
        mb_charclass_set(out, (unsigned char)']');
        p++;
    }

    while (p < end && *p != ']') {
        /* POSIX named class: [:name:] */
        if (*p == '[' && p[1] == ':') {
            const char *cls_start = p + 2;
            const char *cls_end = cls_start;
            while (cls_end < end && *cls_end != ':') cls_end++;
            if (*cls_end == ':' && cls_end[1] == ']') {
                /* Valid POSIX class */
                char name[32];
                size_t nlen = (size_t)(cls_end - cls_start);
                if (nlen < sizeof(name)) {
                    memcpy(name, cls_start, nlen);
                    name[nlen] = '\0';
                    /* Apply to out (we'll negate at the end if needed) */
                    mb_charclass tmp;
                    memset(tmp.bits, 0, sizeof(tmp.bits));
                    apply_posix_class(&tmp, name, 0);
                    /* Merge into out */
                    for (int i = 0; i < 32; i++)
                        out->bits[i] |= tmp.bits[i];
                }
                p = cls_end + 2; /* skip :] */
                continue;
            }
        }

        /* Collating element [. .] — treat as literal char */
        if (*p == '[' && p[1] == '.') {
            p += 2;
            unsigned char lit = (p < end) ? (unsigned char)*p : 0;
            while (p < end && !(*p == '.' && p[1] == ']')) p++;
            if (p < end) p += 2; /* skip .] */
            mb_charclass_set(out, lit);
            continue;
        }

        /* Equivalence class [= =] — treat as literal char */
        if (*p == '[' && p[1] == '=') {
            p += 2;
            unsigned char lit = (p < end) ? (unsigned char)*p : 0;
            while (p < end && !(*p == '=' && p[1] == ']')) p++;
            if (p < end) p += 2; /* skip =] */
            mb_charclass_set(out, lit);
            continue;
        }

        /* Range: a-z */
        if (p + 2 < end && p[1] == '-' && p[2] != ']') {
            unsigned char lo = (unsigned char)*p;
            unsigned char hi = (unsigned char)p[2];
            if (lo > hi) {
                /* Invalid range: treat as three literal chars */
                mb_charclass_set(out, lo);
                mb_charclass_set(out, (unsigned char)'-');
                mb_charclass_set(out, hi);
            } else {
                for (unsigned int c = lo; c <= hi; c++)
                    mb_charclass_set(out, (unsigned char)c);
            }
            p += 3;
            continue;
        }

        /* Literal char (including escaped) */
        unsigned char c;
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = (unsigned char)*p; break;
            }
        } else {
            c = (unsigned char)*p;
        }
        mb_charclass_set(out, c);
        p++;
    }

    /* Apply case-insensitive: for every set char, also set its counterpart */
    if (icase) {
        mb_charclass tmp;
        memcpy(&tmp, out, sizeof(tmp));
        for (int c = 0; c < 256; c++) {
            if (mb_charclass_test(&tmp, (unsigned char)c)) {
                unsigned char lo = (unsigned char)tolower((unsigned char)c);
                unsigned char up = (unsigned char)toupper((unsigned char)c);
                mb_charclass_set(out, lo);
                mb_charclass_set(out, up);
            }
        }
    }

    /* Apply negation */
    if (negate) {
        for (int i = 0; i < 32; i++)
            out->bits[i] ^= 0xFF;
        /* Never match NUL in negated class */
        out->bits[0] &= (uint8_t)~1u;
    }

    return 1;
}
