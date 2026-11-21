/* parse.c — GNU grep BRE/ERE syntax -> AST
 *
 * The grammar and the meaning of every construct follow GNU grep's dfa.c,
 * which is what decides what a GNU grep pattern matches:
 *
 *   regexp  := branch ('|' branch)*
 *   branch  := closure+
 *   closure := atom ('*' | '+' | '?' | '{m,n}')*
 *   atom    := char | set | '.' | anchor | backref | '(' regexp ')' | empty
 *
 * with the context rules of the two syntaxes (RE_SYNTAX_GREP for -G,
 * RE_SYNTAX_EGREP for -E): in a BRE, '*' at the start of an expression and
 * '^'/'$' away from the ends are ordinary characters; in an ERE they are
 * always operators, and a leading repetition applies to the empty string
 * (with the same warning GNU grep prints).
 *
 * GNU grep also runs every pattern through glibc's regex compiler, which
 * rejects some patterns dfa.c would accept; the errors here are the union
 * of both, named the way glibc names them.
 *
 * GNU extensions: \< \> \b \B \w \W \s \S \` \' and \{,n\}.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <string.h>

#define RE_DUP_MAX_ 0x7fff

#define E_EESCAPE "Trailing backslash"
#define E_EPAREN  "Unmatched ( or \\("
#define E_ERPAREN "Unmatched ) or \\)"
#define E_EBRACE  "Unmatched \\{"
#define E_BADBR   "Invalid content of \\{\\}"
#define E_ESIZE   "Regular expression too big"
#define E_ESUBREG "Invalid back reference"
#define E_ESPACE  "Memory exhausted"

enum {
    T_END, T_CHAR, T_SET, T_ANY, T_ASSERT, T_BACKREF,
    T_STAR, T_PLUS, T_QMARK, T_REPMN, T_OR, T_LPAREN, T_RPAREN
};

typedef struct {
    mb_ast      *t;
    const char  *p, *end;
    int          ere, icase;
    int          tok, lasttok, laststart;
    unsigned char c;          /* T_CHAR */
    mb_charclass set;         /* T_SET */
    int          kind;        /* T_ASSERT, T_BACKREF */
    int          minrep, maxrep;
    int          parens;
    int          ngroups;
    int         *gstack;      /* open group numbers */
    int          completed[10];
    int          has_backref;
    const char  *err;
    int          esize;
    int          glibc;       /* parse as glibc's regex does (see mb_parse) */
    int          nlalt;       /* '\n' separates alternatives */
    int          quirk;       /* glibc would read this pattern differently */
    int          rparen_lit;  /* an unmatched ')' was taken literally */
    mb_parsed   *out;         /* libc edits are recorded here */
    const char  *deferred;    /* dfa.c-only error, reported if all else ok */
    const char  *start;
    void       (*warn)(void *, const char *);
    void        *wctx;
} P;

static void warn(P *s, const char *m)
{
    if (s->warn) s->warn(s->wctx, m);
}

/* glibc fetch_token() as far as interval parsing needs it */
enum { FT_END, FT_CHAR, FT_CLOSE, FT_OTHER };

static int ft(P *s, const char **q, unsigned char *c)
{
    const char *r = *q;
    if (r >= s->end) return FT_END;
    if (*r == '\\') {
        if (r + 1 >= s->end) { *q = r + 1; *c = 0; return FT_END; }
        *c = (unsigned char)r[1];
        *q = r + 2;
        if (!s->ere && r[1] == '}') return FT_CLOSE;
        return FT_OTHER;
    }
    *c = (unsigned char)*r;
    *q = r + 1;
    if (s->ere && *r == '}') return FT_CLOSE;
    return FT_CHAR;
}

/* glibc fetch_number(): -1 no digits, -2 invalid, else the number */
static long fetch_number(P *s, const char **q, int *last, unsigned char *lastc)
{
    long num = -1;
    for (;;) {
        unsigned char c;
        int ty = ft(s, q, &c);
        *last = ty; *lastc = c;
        if (ty == FT_END) return -2;
        if (ty == FT_CLOSE || c == ',') break;
        if (ty != FT_CHAR || c < '0' || c > '9' || num == -2)
            num = -2;
        else if (num == -1)
            num = c - '0';
        else {
            num = num * 10 + (c - '0');
            if (num > RE_DUP_MAX_ + 1) num = RE_DUP_MAX_ + 1;
        }
    }
    return num;
}

/* Interval after '{' / '\{'. 1: interval, 0: ordinary '{' (ERE), -1: error */
static int interval(P *s)
{
    const char *q = s->p;
    int lt; unsigned char lc;
    long start = fetch_number(s, &q, &lt, &lc), endv = 0;
    int st_tok = lt; unsigned char st_c = lc;
    if (start == -1) {
        if (st_tok != FT_CLOSE && st_c == ',') start = 0;
        else { s->err = E_BADBR; return -1; }
    }
    if (start != -2) {
        if (st_tok == FT_CLOSE)      endv = start;
        else if (st_c == ',')        endv = fetch_number(s, &q, &lt, &lc);
        else                         endv = -2;
    }
    if (start == -2 || endv == -2) {
        if (!s->ere) {
            s->err = (lt == FT_END) ? E_EBRACE : E_BADBR;
            return -1;
        }
        return 0;
    }
    if ((endv != -1 && start > endv) || lt != FT_CLOSE) {
        s->err = E_BADBR;
        return -1;
    }
    if ((endv == -1 ? start : endv) > RE_DUP_MAX_) {
        s->err = E_ESIZE;
        s->esize = 1;
        return -1;
    }
    s->minrep = (int)start;
    s->maxrep = (int)endv;
    s->p = q;
    return 1;
}

static void set_char(P *s, unsigned char c)
{
    memset(&s->set, 0, sizeof(s->set));
    mb_charclass_set(&s->set, c);
    if (s->icase && isalpha(c)) {
        mb_charclass_set(&s->set, (unsigned char)tolower(c));
        mb_charclass_set(&s->set, (unsigned char)toupper(c));
    }
}

/*
 * GNU grep matches lines with dfa.c but finds the matched text (-o,
 * --color) with glibc's regex, and the two read a repetition operator
 * after a zero-width assertion (or, in an ERE, a leading '{') differently:
 * dfa.c applies it, glibc does not -- in an ERE it drops the operator, in
 * a BRE it is an ordinary character. The pattern is parsed both ways when
 * that matters (quirk); glibc's reading drives -o and --color.
 *
 * glibc_op(): in a BRE under glibc rules, the operator is a literal.
 */
static int lex(P *s);

static int glibc_op(P *s, int brace)
{
    (void)brace;
    if (s->ere) {
        /* glibc drops it: same meaning as dfa.c's "repeat nothing" unless
         * an anchor precedes, it is a '{', or a ')' follows -- let the
         * glibc reading (and its verdict on validity) be computed */
        if (s->lasttok == T_ASSERT || s->lasttok == T_END ||
            s->lasttok == T_LPAREN || s->lasttok == T_OR)
            s->quirk = 1;
        return 0;
    }
    if (s->lasttok != T_ASSERT) return 0;
    s->quirk = 1;
    return s->glibc;
}

/* Record a rewrite of the pattern for POSIX regcomp (back-references are
 * left to libc, whose ERE rejects what GNU syntax reads leniently). */
static void libc_edit(P *s, int kind, const char *at)
{
    mb_parsed *o = s->out;
    if (o->nedits < MB_MAX_EDITS) {
        o->edit_pos[o->nedits] = (size_t)(at - s->start);
        o->edit_kind[o->nedits] = (char)kind;
        o->nedits++;
    } else {
        o->edits_overflow = 1;
    }
}

/* ERE under glibc rules: an operator with nothing to repeat is skipped. */
static int glibc_skip(P *s)
{
    return s->lasttok == T_END || s->lasttok == T_LPAREN ||
           s->lasttok == T_OR  || s->lasttok == T_ASSERT;
}

/* After a skipped operator glibc parses the next token as the start of an
 * expression, where a ')' is an ordinary character (it does not close the
 * group, so "(*)" is "Unmatched ( or \("). */
static int glibc_after_skip(P *s)
{
    if (s->p < s->end && *s->p == ')') {
        s->p++;
        s->laststart = 0;
        s->c = ')';
        return s->lasttok = T_CHAR;
    }
    return lex(s);
}

static int lex(P *s)
{
    int backslash = 0;
    if (s->p >= s->end) return s->lasttok = T_END;
    unsigned char c = (unsigned char)*s->p++;
    if (c == '\\') {
        if (s->p >= s->end) { s->err = E_EESCAPE; return s->lasttok = T_END; }
        backslash = 1;
        c = (unsigned char)*s->p++;
    }

    switch (c) {
    case '\n':
        if (backslash || !s->nlalt) goto normal;
        s->laststart = 1;
        return s->lasttok = T_OR;
    case '^':
        if (backslash) goto normal;
        if (s->ere || s->lasttok == T_END || s->lasttok == T_LPAREN ||
            s->lasttok == T_OR) {
            s->kind = AS_BOL;
            return s->lasttok = T_ASSERT;
        }
        goto normal;
    case '$':
        if (backslash) goto normal;
        if (s->ere || s->p >= s->end ||
            (s->p + 1 < s->end && s->p[0] == '\\' &&
             (s->p[1] == ')' || s->p[1] == '|'))) {
            s->kind = AS_EOL;
            return s->lasttok = T_ASSERT;
        }
        goto normal;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
        if (!backslash) goto normal;
        s->laststart = 0;
        if (!s->completed[c - '0']) { s->err = E_ESUBREG; return s->lasttok = T_END; }
        s->kind = c - '0';
        s->has_backref = 1;
        return s->lasttok = T_BACKREF;
    case '`':
        if (!backslash) goto normal;
        s->kind = AS_BOL;
        return s->lasttok = T_ASSERT;
    case '\'':
        if (!backslash) goto normal;
        s->kind = AS_EOL;
        return s->lasttok = T_ASSERT;
    case '<':
        if (!backslash) goto normal;
        s->kind = AS_WBEG;
        return s->lasttok = T_ASSERT;
    case '>':
        if (!backslash) goto normal;
        s->kind = AS_WEND;
        return s->lasttok = T_ASSERT;
    case 'b':
        if (!backslash) goto normal;
        s->kind = AS_WORDB;
        return s->lasttok = T_ASSERT;
    case 'B':
        if (!backslash) goto normal;
        s->kind = AS_NWORDB;
        return s->lasttok = T_ASSERT;
    case '?':
    case '+':
        if (s->ere == backslash) goto normal;
        if (s->laststart && !s->ere) goto normal;
        if (glibc_op(s, 0)) goto normal;
        if (s->ere && glibc_skip(s)) {
            if (s->glibc) return glibc_after_skip(s);
            libc_edit(s, 'd', s->p - 1);
        }
        if (s->laststart)
            warn(s, c == '?' ? "? at start of expression"
                             : "+ at start of expression");
        return s->lasttok = (c == '?' ? T_QMARK : T_PLUS);
    case '*':
        if (backslash) goto normal;
        if (s->laststart && !s->ere) goto normal;
        if (glibc_op(s, 0)) goto normal;
        if (s->ere && glibc_skip(s)) {
            if (s->glibc) return glibc_after_skip(s);
            libc_edit(s, 'd', s->p - 1);
        }
        if (s->laststart)
            warn(s, "* at start of expression");
        return s->lasttok = T_STAR;
    case '{': {
        if (s->ere == backslash) goto normal;
        if (s->laststart && !s->ere) goto normal;
        if (glibc_op(s, 1)) goto normal;
        if (s->glibc && s->ere && glibc_skip(s)) return glibc_after_skip(s);
        const char *brace = s->p - 1;
        int r = interval(s);
        if (r < 0 && s->ere && s->laststart && !s->esize) {
            /* glibc skips a leading repetition token unvalidated and dfa.c
             * reads an invalid one as text: "{2,1}" at the start is text. */
            s->err = NULL;
            r = 0;
        }
        if (r < 0) return s->lasttok = T_END;
        if (r == 0) goto normal;
        if (s->ere && glibc_skip(s)) libc_edit(s, 'd', brace);
        if (s->laststart) warn(s, "{...} at start of expression");
        s->laststart = 0;
        return s->lasttok = T_REPMN;
    }
    case '|':
        if (s->ere == backslash) goto normal;
        s->laststart = 1;
        return s->lasttok = T_OR;
    case '(':
        if (s->ere == backslash) goto normal;
        s->gstack[s->parens++] = ++s->ngroups;
        s->laststart = 1;
        return s->lasttok = T_LPAREN;
    case ')':
        if (s->ere == backslash) goto normal;
        if (s->parens == 0) {
            if (s->ere) { s->rparen_lit = 1; goto normal; }
            s->err = E_ERPAREN;
            return s->lasttok = T_END;
        }
        {
            int g = s->gstack[--s->parens];
            if (g < 10) s->completed[g] = 1;
        }
        s->laststart = 0;
        return s->lasttok = T_RPAREN;
    case '.':
        if (backslash) goto normal;
        s->laststart = 0;
        memset(&s->set, 0xFF, sizeof(s->set));
        return s->lasttok = T_ANY;
    case 's': case 'S': case 'w': case 'W': {
        if (!backslash) goto normal;
        s->laststart = 0;
        memset(&s->set, 0, sizeof(s->set));
        for (int k = 0; k < 256; k++) {
            int m = (c == 's' || c == 'S') ? isspace(k) != 0 : mb_is_word((unsigned char)k);
            if (c == 'S' || c == 'W') m = !m;
            if (m) mb_charclass_set(&s->set, (unsigned char)k);
        }
        return s->lasttok = T_SET;
    }
    case '[': {
        if (backslash) goto normal;
        s->laststart = 0;
        const char *e = NULL, *err = NULL;
        int r = mb_bracket_parse(s->p, s->end, s->icase, &s->set, &e, &err);
        if (r < 0) { s->err = err; return s->lasttok = T_END; }
        if (r > 0 && !s->deferred) s->deferred = err;
        s->p = e;
        return s->lasttok = T_SET;
    }
    default:
    normal:
        if (c == '{' && s->ere && !backslash) libc_edit(s, 'e', s->p - 1);
        s->laststart = 0;
        s->c = c;
        return s->lasttok = T_CHAR;
    }
}

static int regexp(P *s);

static int atom(P *s)
{
    mb_ast *t = s->t;
    int n;
    switch (s->tok) {
    case T_CHAR:
        set_char(s, s->c);
        n = mb_ast_set(t, &s->set);
        s->tok = lex(s);
        return n;
    case T_SET:
    case T_ANY:
        n = mb_ast_set(t, &s->set);
        s->tok = lex(s);
        return n;
    case T_ASSERT:
        n = mb_ast_assert(t, s->kind);
        s->tok = lex(s);
        return n;
    case T_BACKREF:
        n = mb_ast_node(t, A_BACKREF, -1, -1);
        if (n >= 0) t->n[n].kind = (uint8_t)s->kind;
        s->tok = lex(s);
        return n;
    case T_LPAREN:
        s->tok = lex(s);
        if (s->err) return -1;
        n = regexp(s);
        if (s->err) return -1;
        if (s->tok != T_RPAREN) { s->err = E_EPAREN; return -1; }
        s->tok = lex(s);
        return n;
    default:
        return mb_ast_node(t, A_EMPTY, -1, -1);
    }
}

static int closure(P *s)
{
    int a = atom(s);
    while (!s->err && a >= 0 &&
           (s->tok == T_STAR || s->tok == T_PLUS || s->tok == T_QMARK ||
            s->tok == T_REPMN)) {
        int mn = 0, mx = -1;
        if (s->tok == T_PLUS)  mn = 1;
        if (s->tok == T_QMARK) mx = 1;
        if (s->tok == T_REPMN) { mn = s->minrep; mx = s->maxrep; }
        int r = mb_ast_node(s->t, A_REP, a, -1);
        if (r < 0) return -1;
        s->t->n[r].min = mn;
        s->t->n[r].max = mx;
        a = r;
        s->tok = lex(s);
    }
    return s->err ? -1 : a;
}

static int branch(P *s)
{
    int a = closure(s);
    while (!s->err && a >= 0 && s->tok != T_RPAREN && s->tok != T_OR &&
           s->tok != T_END) {
        int b = closure(s);
        if (b < 0) return -1;
        a = mb_ast_cat(s->t, a, b);
    }
    return s->err ? -1 : a;
}

static int regexp(P *s)
{
    int a = branch(s);
    while (!s->err && a >= 0 && s->tok == T_OR) {
        s->tok = lex(s);
        if (s->err) return -1;
        int b = branch(s);
        if (b < 0) return -1;
        a = mb_ast_alt(s->t, a, b);
    }
    return s->err ? -1 : a;
}

int mb_parse(mb_ast *t, const char *pat, size_t len, int flags,
             mb_parsed *out, const char **err,
             void (*warnfn)(void *ctx, const char *msg), void *wctx)
{
    P s;
    memset(&s, 0, sizeof(s));
    s.t     = t;
    s.p     = pat;
    s.end   = pat + len;
    s.ere   = (flags & SX_REG_ERE) != 0;
    s.icase = (flags & SX_REG_ICASE) != 0;
    s.lasttok   = T_END;
    s.laststart = 1;
    s.warn  = warnfn;
    s.wctx  = wctx;
    s.glibc = (flags & SX_P_GLIBC) != 0;
    s.start = pat;
    s.out   = out;
    out->nedits = 0;
    out->edits_overflow = 0;
    s.nlalt = (flags & SX_P_NLALT) != 0;
    int gsmall[64];
    s.gstack = len < 64 ? gsmall : malloc((len + 1) * sizeof(int));
    if (!s.gstack) { *err = E_ESPACE; return -1; }

    s.tok = lex(&s);
    int root = s.err ? -1 : regexp(&s);
    if (!s.err && root >= 0 && s.tok != T_END)
        s.err = E_EPAREN;               /* not reachable: ')' is handled in lex */
    if (!s.err && root >= 0 && s.parens > 0)
        s.err = E_EPAREN;
    if (!s.err && t->oom)
        s.err = E_ESPACE;
    if (!s.err && s.deferred)
        s.err = s.deferred;
    if (s.gstack != gsmall) free(s.gstack);

    if (s.err || root < 0) {
        *err = s.err ? s.err : E_ESPACE;
        return -1;
    }
    out->root = root;
    out->has_backref = s.has_backref;
    out->quirk = s.quirk;
    out->rparen_lit = s.rparen_lit;
    return 0;
}

int mb_parse_fixed(mb_ast *t, const char *pat, size_t len, int icase)
{
    int a = mb_ast_node(t, A_EMPTY, -1, -1);
    for (size_t i = 0; i < len && a >= 0; i++) {
        mb_charclass cc;
        unsigned char c = (unsigned char)pat[i];
        memset(&cc, 0, sizeof(cc));
        mb_charclass_set(&cc, c);
        if (icase && isalpha(c)) {
            mb_charclass_set(&cc, (unsigned char)tolower(c));
            mb_charclass_set(&cc, (unsigned char)toupper(c));
        }
        a = mb_ast_cat(t, a, mb_ast_set(t, &cc));
    }
    return a;
}
