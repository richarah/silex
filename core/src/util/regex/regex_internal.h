/* regex_internal.h — internal types shared across the regex engine modules
 *
 * Pipeline:
 *
 *   parse.c       pattern text (GNU grep BRE/ERE syntax) -> AST
 *   charclass_re.c  bracket expressions -> 256-bit byte sets
 *   compile.c     AST -> Thompson NFA program
 *   classify.c    AST -> required literals ("musts") for the prefilter
 *   literal.c     SIMD literal search, newline counting
 *   thompson.c    lazy DFA built from the NFA (match-only, scanning, bounds)
 *   mb_regex.c    public API: pattern sets, line scanning, -o/-w iteration,
 *                 libc regexec only for patterns with back-references
 */
#ifndef SILEX_REGEX_INTERNAL_H
#define SILEX_REGEX_INTERNAL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex.h"
#include "../section.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

/* ---- Byte sets ------------------------------------------------------------ */

typedef struct {
    uint8_t bits[32];   /* 256-bit bitmap, one bit per byte value */
} mb_charclass;

static inline int mb_charclass_test(const mb_charclass *cc, unsigned char c) {
    return (cc->bits[c >> 3] >> (c & 7)) & 1;
}

static inline void mb_charclass_set(mb_charclass *cc, unsigned char c) {
    cc->bits[c >> 3] |= (uint8_t)(1u << (c & 7));
}

/* Word constituent for \b \< \> \w and -w: [A-Za-z0-9_] (C locale). */
static inline int mb_is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/*
 * Parse a bracket expression. p points just after '['; end is the end of
 * the pattern. On success fills *out, sets *endp just past the closing ']'
 * and returns 0. On error returns -1 with *err set to a static message
 * (the text glibc/GNU grep use for the same error).
 */
int mb_bracket_parse(const char *p, const char *end, int icase,
                     mb_charclass *out, const char **endp, const char **err);

/* ---- AST ------------------------------------------------------------------- */

enum {
    A_EMPTY,     /* matches the empty string */
    A_SET,       /* one byte from set[set] */
    A_CAT,       /* a then b */
    A_ALT,       /* a or b */
    A_REP,       /* a{min,max}, max < 0: unbounded */
    A_ASSERT,    /* zero-width assertion, kind = AS_* */
    A_BACKREF,   /* \N (only ever matched by libc) */
};

enum {
    AS_BOL = 1,  /* ^ \`               */
    AS_EOL,      /* $ \'               */
    AS_WORDB,    /* \b                 */
    AS_NWORDB,   /* \B                 */
    AS_WBEG,     /* \<                 */
    AS_WEND,     /* \>                 */
    AS_NOTWPREV, /* -w: no word char before */
    AS_NOTWNEXT, /* -w: no word char after  */
};

typedef struct {
    uint8_t type;
    uint8_t kind;    /* A_ASSERT kind; A_BACKREF number */
    int     a, b;    /* children */
    int     min, max;
    int     set;     /* A_SET: index into ast.sets */
} ast_node;

typedef struct {
    ast_node     *n;
    int           len, cap;
    mb_charclass *sets;
    int           nsets, setcap;
    int           oom;
} mb_ast;

void mb_ast_init(mb_ast *t);
void mb_ast_free(mb_ast *t);
int  mb_ast_node(mb_ast *t, int type, int a, int b);
int  mb_ast_set(mb_ast *t, const mb_charclass *cc);  /* returns A_SET node */
int  mb_ast_assert(mb_ast *t, int kind);
int  mb_ast_cat(mb_ast *t, int a, int b);             /* a < 0 / b < 0: other */
int  mb_ast_alt(mb_ast *t, int a, int b);
int  mb_ast_nullable(const mb_ast *t, int i);
int  mb_ast_has_assert(const mb_ast *t, int i);

#define MB_MAX_EDITS 32

/* Parse result for one pattern */
typedef struct {
    int root;          /* AST node, or -1 on error */
    int has_backref;   /* pattern needs the libc engine */
    int quirk;         /* glibc's reading differs (see parse.c) */
    int rparen_lit;    /* ERE: an unmatched ')' is a literal */
    /* rewrites for libc regcomp: 'e' escape the byte, 'd' delete it */
    int    nedits, edits_overflow;
    size_t edit_pos[MB_MAX_EDITS];
    char   edit_kind[MB_MAX_EDITS];
} mb_parsed;

/* internal mb_parse flags */
#define SX_P_GLIBC (1 << 20)   /* parse the way glibc regcomp does */
#define SX_P_NLALT (1 << 21)   /* newline separates alternatives */

/*
 * Parse one pattern (no newlines) into t. flags: SX_REG_ERE, SX_REG_ICASE.
 * Returns 0 on success; on error -1 with *err set. warn (may be NULL) is
 * called for GNU grep's "* at start of expression"-style warnings.
 */
int mb_parse(mb_ast *t, const char *pat, size_t len, int flags,
             mb_parsed *out, const char **err,
             void (*warn)(void *ctx, const char *msg), void *wctx);

/* Literal pattern (-F) as an AST */
int mb_parse_fixed(mb_ast *t, const char *pat, size_t len, int icase);

/* ---- NFA ------------------------------------------------------------------- */

enum { N_SET, N_SPLIT, N_JMP, N_ASSERT, N_MATCH };

typedef struct {
    uint8_t op;
    uint8_t kind;    /* N_ASSERT */
    int     set;     /* N_SET: index into prog.sets */
    int     x, y;    /* successors (SPLIT: both) */
} mb_inst;

#define SX_MAX_INSTRS (1 << 21)

typedef struct {
    mb_inst      *in;
    int           len, cap;
    int           start;
    mb_charclass *sets;     /* shared with the AST (not owned) */
    int           nsets;
    int           asserts;  /* bitmask of 1 << AS_* used */
    int           too_big;
} mb_prog;

int  mb_compile(const mb_ast *t, int root, mb_prog *p);
void mb_prog_free(mb_prog *p);

/* ---- Required literals (prefilter) ----------------------------------------- */

#define MB_LIT_MAX   8      /* most alternatives the prefilter searches for */

typedef struct {
    int      n;                     /* 0: no prefilter */
    int      icase;
    int      exact;                 /* match <=> one of the literals occurs */
    char    *s[MB_LIT_MAX];
    size_t   len[MB_LIT_MAX];
    /* two probe positions per literal for the SIMD scan */
    size_t   o1[MB_LIT_MAX], o2[MB_LIT_MAX];
    uint8_t  b1[MB_LIT_MAX], b2[MB_LIT_MAX];
    uint8_t  f1[MB_LIT_MAX], f2[MB_LIT_MAX];   /* 0x20 if case-folded */
    size_t   maxprobe;
    size_t   maxlen;
} mb_litset;

/*
 * Compute a literal set for the alternation of roots[0..n-1]: every match
 * of any of them contains one of the literals. exact is set when the
 * patterns match exactly the literals (no other syntax).
 */
void mb_musts(const mb_ast *t, const int *roots, int n, int icase,
              mb_litset *out);
void mb_litset_free(mb_litset *ls);
char *mb_ast_plain(const mb_ast *t, int root, int icase, size_t *len,
                   int *asserts);
void mb_litset_prepare(mb_litset *ls);

/* Leftmost occurrence of any literal in [p, end); NULL if none. */
const char *mb_lit_find(const mb_litset *ls, const char *p, const char *end);

/* Start of the line holding buf[at] (search back no further than lo) */
size_t mb_line_start(const char *buf, size_t lo, size_t at);

/* Count '\n' bytes in [p, p+n) */
size_t mb_count_nl(const char *p, size_t n);

/* ---- Lazy DFA ---------------------------------------------------------------- */

/* Previous-byte context of a position */
enum { CTX_BOL = 0, CTX_NONWORD = 1, CTX_WORD = 2 };

typedef struct mb_dfa mb_dfa;

mb_dfa *mb_dfa_new(const mb_prog *p, int anchored);
void    mb_dfa_free(mb_dfa *d);

/* Does any match occur in s[0..n)? prev is the context of s[-1]. */
int mb_dfa_line(mb_dfa *d, const unsigned char *s, size_t n, int prev);

/*
 * Scan buf[0..n) -- whole lines, the last one possibly unterminated -- for
 * the first line containing a match. Returns 1 and sets *hit to the offset
 * of a byte of that line (or of its '\n', or n for the last line).
 */
int mb_dfa_scan(mb_dfa *d, const unsigned char *buf, size_t n, size_t *hit);


/* Every line matches */
int mb_dfa_all_lines(mb_dfa *d);

/* Earliest end of a match in s[from..n) (unanchored); -1 if none. */
long mb_dfa_first_end(mb_dfa *d, const unsigned char *s, size_t n, size_t from);

/*
 * Anchored at `from`: report match ends. If ends != NULL, every end is
 * stored (ascending) and the count returned; otherwise the longest end is
 * returned. -1 / 0 when there is no match at from.
 */
long mb_dfa_anchored(mb_dfa *d, const unsigned char *s, size_t n, size_t from,
                     size_t *ends, size_t *nends);

static inline int mb_ctx_at(const unsigned char *s, size_t pos) {
    if (pos == 0) return CTX_BOL;
    return mb_is_word(s[pos - 1]) ? CTX_WORD : CTX_NONWORD;
}

/* ---- mb_regex struct ----------------------------------------------------------- */

typedef struct {
    regex_t re;
    int     ok;
} mb_posix;

struct mb_regex {
    int        flags;
    int        nothing;      /* no patterns: never matches */
    mb_ast     ast;
    /* DFA side: all patterns without back-references */
    int        have_dfa;
    mb_prog    sel_prog;     /* with -w / -x wrapping: selects lines */
    mb_prog    in_prog;      /* plain alternation: match bounds */
    mb_dfa    *sel;          /* unanchored, sel_prog */
    mb_dfa    *in_un;        /* unanchored, in_prog */
    mb_dfa    *in_an;        /* anchored,   in_prog */
    int        sel_is_in;    /* sel_prog == in_prog (no -w/-x) */
    int        in_root, sel_root;
    int        progs_state;  /* 0 not compiled, 1 compiled, -1 failed */
    int        quirk;        /* in_prog is glibc's reading, sel dfa.c's */
    /* libc side: patterns with back-references */
    mb_posix  *posix;
    int        nposix;
    /* every pattern a plain string (and no back-references): -w and -x are
     * checked on the literal hits, no DFA needed */
    mb_litset  plain;
    int        plain_ok;
    unsigned char pbol[MB_LIT_MAX], peol[MB_LIT_MAX];   /* ^lit / lit$ */
    /* prefilter */
    mb_litset  lits;
    int        pf_on;        /* prefilter single lines */
    int        pf_scan;      /* prefilter buffer scans */
    size_t     pf_scanned, pf_verified;
    unsigned   first_char;
    /* scratch for anchored match ends */
    size_t    *ends;
    size_t     ends_cap;
};

#endif /* SILEX_REGEX_INTERNAL_H */
