/* regex_internal.h — internal types shared across the regex engine modules */
#ifndef MATCHBOX_REGEX_INTERNAL_H
#define MATCHBOX_REGEX_INTERNAL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex.h"
#include "../section.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

/* ---- Pattern classification --------------------------------------------- */

typedef enum {
    MB_CLASS_FIXED,      /* no metacharacters → Boyer-Moore-Horspool */
    MB_CLASS_PREFIX,     /* ^literal → memcmp at line start */
    MB_CLASS_ANCHORED,   /* ^literal$ → strcmp whole line */
    MB_CLASS_CHARCLASS,  /* single [class] only → bitmap lookup */
    MB_CLASS_SIMPLE,     /* no backrefs → Thompson NFA/DFA */
    MB_CLASS_BACKREF,    /* has \1-\9 → regexec fallback */
} mb_class_t;

mb_class_t mb_classify(const char *pat, int flags);
int        mb_extract_fixed(const char *pat, mb_class_t class,
                             char *buf, size_t buf_size);

/* ---- Character class bitmap --------------------------------------------- */

typedef struct {
    uint8_t bits[32];   /* 256-bit bitmap, one bit per ASCII/byte value */
} mb_charclass;

int mb_charclass_parse(const char *pat, const char *end,
                       mb_charclass *out, int flags);

static inline int mb_charclass_test(const mb_charclass *cc, unsigned char c) {
    return (cc->bits[c >> 3] >> (c & 7)) & 1;
}

static inline void mb_charclass_set(mb_charclass *cc, unsigned char c) {
    cc->bits[c >> 3] |= (uint8_t)(1u << (c & 7));
}

/* ---- NFA instruction set ------------------------------------------------- */

/* Max instructions per compiled pattern */
#define MB_MAX_INSTRS 4096

typedef enum {
    I_MATCH,    /* success */
    I_CHAR,     /* match literal char (arg.c) */
    I_ICHAR,    /* match char case-insensitively (arg.c = lower(c)) */
    I_CLASS,    /* match char in bitmap (arg.cc) */
    I_ANY,      /* match any char (except \n if NEWLINE mode) */
    I_SPLIT,    /* NFA fork: goto x and y */
    I_JUMP,     /* deterministic goto x */
    I_BOL,      /* assert beginning of line */
    I_EOL,      /* assert end of line */
    I_SAVE,     /* save position (arg.save_slot) — only if !NOSUB */
} instr_type_t;

typedef struct {
    instr_type_t op;
    int x, y;           /* successor state indices for SPLIT/JUMP */
    union {
        char         c;
        mb_charclass *cc;
        int          save_slot;
    } arg;
} mb_instr;

/* NFA program */
typedef struct {
    mb_instr *instrs;
    int       len;
    int       cap;
} mb_prog;

void mb_prog_init(mb_prog *p);
void mb_prog_free(mb_prog *p);
int  mb_prog_emit(mb_prog *p, mb_instr instr);  /* returns instr index or -1 */
int  mb_prog_emit_class(mb_prog *p, mb_charclass *cc,
                        instr_type_t op);        /* copies cc, returns index */

/* ---- Parser (BRE/ERE → NFA program) ------------------------------------- */

/* Parse pattern into *prog. Returns 0 on success, -1 on error.
 * errstr is set to a static error string on failure. */
int mb_parse(const char *pat, int flags, mb_prog *prog,
             const char **errstr);

/* ---- Thompson NFA/DFA simulator ----------------------------------------- */

/* Simulate NFA/DFA search on text.
 * Returns 1 if matched, fills *out on match (if non-NULL).
 * anchor_bol: if 1, only try from position 0 (^ anchored). */
int mb_thompson_search(const mb_prog *prog, int flags,
                       const char *text, size_t n,
                       mb_match *out, int anchor_bol);

/* ---- Boyer-Moore-Horspool ----------------------------------------------- */

void mb_bmh_build(const char *needle, size_t nlen, size_t skip[256]);
const char *mb_bmh_search(const char *haystack, size_t hlen,
                           const char *needle, size_t nlen,
                           const size_t skip[256]);

/* Case-insensitive BMH */
void mb_bmh_build_icase(const char *needle, size_t nlen, size_t skip[256]);
const char *mb_bmh_search_icase(const char *haystack, size_t hlen,
                                 const char *needle, size_t nlen,
                                 const size_t skip[256]);

/* ---- mb_regex struct ----------------------------------------------------- */

struct mb_regex {
    mb_class_t  class;
    int         flags;

    /* MB_CLASS_FIXED / PREFIX / ANCHORED: BMH data */
    char        fixed_str[512];
    size_t      fixed_len;
    size_t      bmh_skip[256];

    /* MB_CLASS_SIMPLE / CHARCLASS: NFA program */
    mb_prog     prog;

    /* MB_CLASS_BACKREF: POSIX fallback */
    regex_t     posix_re;
    int         posix_ok;
};

#endif /* MATCHBOX_REGEX_INTERNAL_H */
