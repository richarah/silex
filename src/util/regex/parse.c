/* parse.c — BRE/ERE → NFA via postfix/shunting-yard + Thompson construction */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

/* ---- Tokens ---------------------------------------------------------------- */

typedef enum {
    TOK_LITERAL,  /* a literal char or ICHAR */
    TOK_CLASS,    /* [...]  */
    TOK_DOT,      /* .      */
    TOK_BOL,      /* ^      */
    TOK_EOL,      /* $      */
    TOK_STAR,     /* *      */
    TOK_PLUS,     /* +      */
    TOK_QUEST,    /* ?      */
    TOK_PIPE,     /* |      */
    TOK_LPAREN,   /* (      */
    TOK_RPAREN,   /* )      */
    TOK_CONCAT,   /* synthetic concatenation operator */
} tok_t;

typedef struct {
    tok_t        type;
    char         c;         /* for LITERAL */
    int          is_ichar;  /* 1 if case-insensitive char */
    mb_charclass cc;        /* for CLASS */
} token;

#define MAX_TOKENS 8192

/* ---- Tokenizer ------------------------------------------------------------- */

typedef struct {
    const char  *p;
    const char  *end;
    int          ere;
    int          icase;
    int          flags;
    const char **errstr;
    token        buf[MAX_TOKENS];
    int          n;
    int          err;
} tokenizer_t;

static int tok_push(tokenizer_t *t, token tok)
{
    if (t->n >= MAX_TOKENS) {
        if (t->errstr) *t->errstr = "pattern too long";
        t->err = 1;
        return -1;
    }
    t->buf[t->n++] = tok;
    return 0;
}

/* Consume a character class [...] starting after the '[' */
static int tokenize_class(tokenizer_t *t)
{
    const char *cls_start = t->p;
    const char *p = t->p;

    /* ']' and '^]' as first (after optional '^') are literal */
    if (*p == '^') p++;
    if (*p == ']') p++;

    /* Scan for matching ] */
    while (p < t->end && *p != ']') {
        if (*p == '[' && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
            /* POSIX class/collating — find closing ]] */
            char closer = p[1];
            p += 2;
            while (p < t->end && !(*p == closer && p[1] == ']')) p++;
            if (p < t->end) p += 2;
        } else if (*p == '\\' && p + 1 < t->end) {
            p += 2;
        } else {
            p++;
        }
    }
    const char *cls_end = p;  /* points to ']' or end */
    if (*p == ']') p++;
    t->p = p;

    token tok;
    memset(&tok, 0, sizeof(tok));
    tok.type = TOK_CLASS;
    mb_charclass_parse(cls_start, cls_end, &tok.cc, t->flags);
    return tok_push(t, tok);
}

/* Main tokenizer: consume one logical token from t->p */
static int tokenize_one(tokenizer_t *t)
{
    if (t->p >= t->end) return 0;

    char c = *t->p++;
    token tok;
    memset(&tok, 0, sizeof(tok));

    /* Escape sequence */
    if (c == '\\' && t->p < t->end) {
        char n = *t->p++;
        if (!t->ere) {
            /* BRE metacharacters introduced by backslash */
            if (n == '(') { tok.type = TOK_LPAREN; return tok_push(t, tok); }
            if (n == ')') { tok.type = TOK_RPAREN; return tok_push(t, tok); }
            if (n == '|') { tok.type = TOK_PIPE;   return tok_push(t, tok); }
            if (n == '+') { tok.type = TOK_PLUS;   return tok_push(t, tok); }
            if (n == '?') { tok.type = TOK_QUEST;  return tok_push(t, tok); }
            if (n == '{') {
                /* BRE {n,m}: skip to \} and approximate as PLUS */
                while (t->p + 1 < t->end && !(t->p[0] == '\\' && t->p[1] == '}'))
                    t->p++;
                if (t->p + 1 < t->end) t->p += 2;
                tok.type = TOK_PLUS;
                return tok_push(t, tok);
            }
        } else {
            /* ERE: \1-\9 are backreferences (caller should have used POSIX) */
            if (n >= '1' && n <= '9') {
                /* Emit as literal (approximation — backref in SIMPLE class won't occur) */
                tok.type     = TOK_LITERAL;
                tok.c        = n;
                tok.is_ichar = 0;
                return tok_push(t, tok);
            }
        }
        /* Standard escapes */
        char lit;
        switch (n) {
        case 'n': lit = '\n'; break;
        case 't': lit = '\t'; break;
        case 'r': lit = '\r'; break;
        case 'a': lit = '\a'; break;
        case 'f': lit = '\f'; break;
        case 'v': lit = '\v'; break;
        default:  lit = n;   break;
        }
        tok.type     = TOK_LITERAL;
        tok.c        = t->icase ? (char)tolower((unsigned char)lit) : lit;
        tok.is_ichar = t->icase;
        return tok_push(t, tok);
    }

    /* ERE metacharacters (unescaped) */
    if (t->ere) {
        if (c == '(') { tok.type = TOK_LPAREN; return tok_push(t, tok); }
        if (c == ')') { tok.type = TOK_RPAREN; return tok_push(t, tok); }
        if (c == '|') { tok.type = TOK_PIPE;   return tok_push(t, tok); }
        if (c == '+') { tok.type = TOK_PLUS;   return tok_push(t, tok); }
        if (c == '?') { tok.type = TOK_QUEST;  return tok_push(t, tok); }
        if (c == '{') {
            /* ERE {n,m}: skip to closing } and approximate as PLUS */
            while (t->p < t->end && *t->p != '}') t->p++;
            if (t->p < t->end) t->p++; /* consume } */
            tok.type = TOK_PLUS;
            return tok_push(t, tok);
        }
    }

    /* BRE: * is metachar; + ? { are literal unless preceded by \ */
    if (!t->ere && c == '*') { tok.type = TOK_STAR; return tok_push(t, tok); }

    /* Common metacharacters */
    if (c == '.') { tok.type = TOK_DOT; return tok_push(t, tok); }
    if (c == '^') { tok.type = TOK_BOL; return tok_push(t, tok); }
    if (c == '$') { tok.type = TOK_EOL; return tok_push(t, tok); }
    if (t->ere && c == '*') { tok.type = TOK_STAR; return tok_push(t, tok); }
    if (c == '[') return tokenize_class(t);

    /* Literal */
    tok.type     = TOK_LITERAL;
    tok.c        = t->icase ? (char)tolower((unsigned char)c) : c;
    tok.is_ichar = t->icase;
    return tok_push(t, tok);
}

/* Full tokenization of the regex string */
static int tokenize(tokenizer_t *t, const char *pat, int flags,
                    const char **errstr)
{
    t->p      = pat;
    t->end    = pat + strlen(pat);
    t->ere    = (flags & SX_REG_ERE) != 0;
    t->icase  = (flags & SX_REG_ICASE) != 0;
    t->flags  = flags;
    t->errstr = errstr;
    t->n      = 0;
    t->err    = 0;

    while (t->p < t->end && !t->err) {
        tokenize_one(t);
    }
    return t->err ? -1 : 0;
}

/* ---- Insert explicit concatenation operators ------------------------------ */
/*
 * After an atom (LITERAL, CLASS, DOT, EOL) or a postfix op (STAR/PLUS/QUEST)
 * or RPAREN, if the next token starts another atom or is LPAREN or BOL,
 * insert a CONCAT between them.
 */
static int insert_concat(token *in, int n_in, token *out, int *n_out)
{
    *n_out = 0;

    for (int i = 0; i < n_in; i++) {
        out[(*n_out)++] = in[i];
        if (*n_out > MAX_TOKENS - 2) return -1;

        if (i + 1 >= n_in) break;

        int left_atom = (in[i].type == TOK_LITERAL ||
                         in[i].type == TOK_CLASS   ||
                         in[i].type == TOK_DOT     ||
                         in[i].type == TOK_EOL     ||
                         in[i].type == TOK_RPAREN  ||
                         in[i].type == TOK_STAR    ||
                         in[i].type == TOK_PLUS    ||
                         in[i].type == TOK_QUEST);

        int right_atom = (in[i+1].type == TOK_LITERAL ||
                          in[i+1].type == TOK_CLASS   ||
                          in[i+1].type == TOK_DOT     ||
                          in[i+1].type == TOK_BOL     ||
                          in[i+1].type == TOK_LPAREN);

        if (left_atom && right_atom) {
            token concat;
            memset(&concat, 0, sizeof(concat));
            concat.type = TOK_CONCAT;
            out[(*n_out)++] = concat;
        }
    }
    return 0;
}

/* ---- Shunting-yard: infix → postfix --------------------------------------- */
/*
 * Operator precedence (higher = tighter binding):
 *   STAR/PLUS/QUEST: 3
 *   CONCAT:          2
 *   PIPE:            1
 */
static int prec(tok_t t) {
    if (t == TOK_STAR || t == TOK_PLUS || t == TOK_QUEST) return 3;
    if (t == TOK_CONCAT) return 2;
    if (t == TOK_PIPE)   return 1;
    return 0;
}
static int is_left_assoc(tok_t t) { (void)t; return 1; }

static int infix_to_postfix(token *in, int n_in, token *out, int *n_out,
                             const char **errstr)
{
    token op_stack[MAX_TOKENS];
    int   op_top = 0;
    *n_out = 0;

    for (int i = 0; i < n_in; i++) {
        tok_t ty = in[i].type;

        if (ty == TOK_LITERAL || ty == TOK_CLASS ||
            ty == TOK_DOT     || ty == TOK_BOL   || ty == TOK_EOL) {
            /* Operand: push to output */
            if (*n_out >= MAX_TOKENS) { if (errstr) *errstr = "pattern too complex"; return -1; }
            out[(*n_out)++] = in[i];
        } else if (ty == TOK_STAR || ty == TOK_PLUS || ty == TOK_QUEST ||
                   ty == TOK_CONCAT || ty == TOK_PIPE) {
            /* Operator: pop higher-or-equal precedence ops to output */
            while (op_top > 0) {
                tok_t top = op_stack[op_top - 1].type;
                if (top != TOK_LPAREN &&
                    (prec(top) > prec(ty) ||
                     (prec(top) == prec(ty) && is_left_assoc(ty)))) {
                    if (*n_out >= MAX_TOKENS) { if (errstr) *errstr = "pattern too complex"; return -1; }
                    out[(*n_out)++] = op_stack[--op_top];
                } else break;
            }
            if (op_top >= MAX_TOKENS) { if (errstr) *errstr = "pattern too complex"; return -1; }
            op_stack[op_top++] = in[i];
        } else if (ty == TOK_LPAREN) {
            if (op_top >= MAX_TOKENS) { if (errstr) *errstr = "pattern too complex"; return -1; }
            op_stack[op_top++] = in[i];
        } else if (ty == TOK_RPAREN) {
            /* Pop until LPAREN */
            while (op_top > 0 && op_stack[op_top - 1].type != TOK_LPAREN) {
                if (*n_out >= MAX_TOKENS) { if (errstr) *errstr = "pattern too complex"; return -1; }
                out[(*n_out)++] = op_stack[--op_top];
            }
            if (op_top == 0) { if (errstr) *errstr = "unmatched )"; return -1; }
            op_top--; /* pop LPAREN */
        }
    }

    /* Pop remaining operators */
    while (op_top > 0) {
        if (op_stack[op_top - 1].type == TOK_LPAREN) {
            if (errstr) *errstr = "unmatched (";
            return -1;
        }
        if (*n_out >= MAX_TOKENS) { if (errstr) *errstr = "pattern too complex"; return -1; }
        out[(*n_out)++] = op_stack[--op_top];
    }
    return 0;
}

/* ---- Fragment stack + Thompson NFA builder -------------------------------- */

/* "Hole" = instruction index + which output field (0=x, 1=y) needs patching.
 * Using dynamic allocation avoids stack overflow when the fragment stack
 * is stored on the heap. */
typedef struct { int idx; int use_y; } hole_t;

typedef struct {
    hole_t *holes;
    int     n;
    int     cap;
} hole_list;

static void hl_init(hole_list *h) { h->holes = NULL; h->n = 0; h->cap = 0; }

static void hl_free(hole_list *h) { free(h->holes); h->holes = NULL; h->n = h->cap = 0; }

static int hl_push(hole_list *h, int idx, int use_y) {
    if (h->n >= h->cap) {
        int nc = h->cap ? h->cap * 2 : 4;
        hole_t *tmp = realloc(h->holes, (size_t)nc * sizeof(hole_t));
        if (!tmp) return -1;
        h->holes = tmp;
        h->cap   = nc;
    }
    h->holes[h->n].idx   = idx;
    h->holes[h->n].use_y = use_y;
    h->n++;
    return 0;
}

static void hl_patch(mb_prog *prog, hole_list *h, int target) {
    for (int i = 0; i < h->n; i++) {
        if (h->holes[i].use_y)
            prog->instrs[h->holes[i].idx].y = target;
        else
            prog->instrs[h->holes[i].idx].x = target;
    }
}

static int hl_merge(hole_list *dst, hole_list *src) {
    for (int i = 0; i < src->n; i++) {
        if (hl_push(dst, src->holes[i].idx, src->holes[i].use_y) < 0)
            return -1;
    }
    return 0;
}

/* Move src into dst (dst gets src's buffer, src is cleared) */
static void hl_move(hole_list *dst, hole_list *src) {
    hl_free(dst);
    *dst = *src;
    hl_init(src);
}

typedef struct {
    int       start;   /* entry instruction index */
    hole_list out;     /* unpatched outputs */
    int       valid;
} frag;

#define MAX_FSTACK 256

static int emit1(mb_prog *prog, instr_type_t op, char c, int x, int y) {
    mb_instr in;
    memset(&in, 0, sizeof(in));
    in.op    = op;
    in.arg.c = c;
    in.x     = x;
    in.y     = y;
    return mb_prog_emit(prog, in);
}

static int emit_class_instr(mb_prog *prog, mb_charclass *cc) {
    return mb_prog_emit_class(prog, cc, I_CLASS);
}

static frag build_nfa(token *post, int n_post, mb_prog *prog,
                      const char **errstr)
{
    /* Allocate fragment stack on heap to avoid stack overflow
     * (each frag has a dynamic hole_list; MAX_FSTACK static frags ≈ safe) */
    frag *stack = calloc(MAX_FSTACK, sizeof(frag));
    if (!stack) {
        if (errstr) *errstr = "out of memory";
        frag bad; bad.valid = 0; hl_init(&bad.out); return bad;
    }
    int  stk = 0;

#define PUSH(f) do { if (stk >= MAX_FSTACK) goto too_complex; stack[stk++] = (f); } while(0)
#define POP()   (stack[--stk])
#define PEEK()  (stack[stk-1])

    for (int i = 0; i < n_post; i++) {
        token *tok = &post[i];

        switch (tok->type) {
        case TOK_LITERAL: {
            instr_type_t op = tok->is_ichar ? I_ICHAR : I_CHAR;
            int idx = emit1(prog, op, tok->c, -1, 0);
            if (idx < 0) goto too_complex;
            frag f; f.valid = 1; f.start = idx;
            memset(&f.out, 0, sizeof(f.out));
            hl_push(&f.out, idx, 0);  /* x is the output hole */
            PUSH(f);
            break;
        }
        case TOK_CLASS: {
            int idx = emit_class_instr(prog, (mb_charclass*)&tok->cc);
            if (idx < 0) goto too_complex;
            frag f; f.valid = 1; f.start = idx;
            memset(&f.out, 0, sizeof(f.out));
            hl_push(&f.out, idx, 0);
            PUSH(f);
            break;
        }
        case TOK_DOT: {
            int idx = emit1(prog, I_ANY, 0, -1, 0);
            if (idx < 0) goto too_complex;
            frag f; f.valid = 1; f.start = idx;
            memset(&f.out, 0, sizeof(f.out));
            hl_push(&f.out, idx, 0);
            PUSH(f);
            break;
        }
        case TOK_BOL: {
            int idx = emit1(prog, I_BOL, 0, -1, 0);
            if (idx < 0) goto too_complex;
            frag f; f.valid = 1; f.start = idx;
            memset(&f.out, 0, sizeof(f.out));
            hl_push(&f.out, idx, 0);
            PUSH(f);
            break;
        }
        case TOK_EOL: {
            int idx = emit1(prog, I_EOL, 0, -1, 0);
            if (idx < 0) goto too_complex;
            frag f; f.valid = 1; f.start = idx;
            memset(&f.out, 0, sizeof(f.out));
            hl_push(&f.out, idx, 0);
            PUSH(f);
            break;
        }
        case TOK_CONCAT: {
            if (stk < 2) goto bad_pattern;
            frag e2 = POP();
            frag e1 = POP();
            /* Patch e1's outputs to e2.start, then free e1.out */
            hl_patch(prog, &e1.out, e2.start);
            hl_free(&e1.out);
            /* f.out = e2.out (transfer ownership) */
            frag f; f.valid = 1; f.start = e1.start;
            hl_init(&f.out); hl_move(&f.out, &e2.out);
            PUSH(f);
            break;
        }
        case TOK_PIPE: {
            if (stk < 2) goto bad_pattern;
            frag e2 = POP();
            frag e1 = POP();
            /* Emit SPLIT(e1.start, e2.start) */
            int idx = emit1(prog, I_SPLIT, 0, e1.start, e2.start);
            if (idx < 0) { hl_free(&e1.out); hl_free(&e2.out); goto too_complex; }
            frag f; f.valid = 1; f.start = idx;
            hl_init(&f.out);
            if (hl_merge(&f.out, &e1.out) < 0) {
                hl_free(&e1.out); hl_free(&e2.out); hl_free(&f.out); goto too_complex;
            }
            if (hl_merge(&f.out, &e2.out) < 0) {
                hl_free(&e1.out); hl_free(&e2.out); hl_free(&f.out); goto too_complex;
            }
            hl_free(&e1.out);
            hl_free(&e2.out);
            PUSH(f);
            break;
        }
        case TOK_STAR: {
            if (stk < 1) goto bad_pattern;
            frag e = POP();
            /* SPLIT(e.start, hole) */
            int idx = emit1(prog, I_SPLIT, 0, e.start, -1);
            if (idx < 0) { hl_free(&e.out); goto too_complex; }
            /* Patch e's outputs back to SPLIT, then free */
            hl_patch(prog, &e.out, idx);
            hl_free(&e.out);
            frag f; f.valid = 1; f.start = idx;
            hl_init(&f.out);
            hl_push(&f.out, idx, 1);  /* split.y is the output hole */
            PUSH(f);
            break;
        }
        case TOK_PLUS: {
            if (stk < 1) goto bad_pattern;
            frag e = POP();
            /* SPLIT(e.start, hole) — entry remains e.start */
            int idx = emit1(prog, I_SPLIT, 0, e.start, -1);
            if (idx < 0) { hl_free(&e.out); goto too_complex; }
            /* Patch e's outputs to SPLIT, then free */
            hl_patch(prog, &e.out, idx);
            hl_free(&e.out);
            frag f; f.valid = 1; f.start = e.start;
            hl_init(&f.out);
            hl_push(&f.out, idx, 1);  /* split.y is the output hole */
            PUSH(f);
            break;
        }
        case TOK_QUEST: {
            if (stk < 1) goto bad_pattern;
            frag e = POP();
            /* SPLIT(e.start, hole) */
            int idx = emit1(prog, I_SPLIT, 0, e.start, -1);
            if (idx < 0) { hl_free(&e.out); goto too_complex; }
            frag f; f.valid = 1; f.start = idx;
            hl_init(&f.out);
            hl_push(&f.out, idx, 1);  /* split.y = hole */
            if (hl_merge(&f.out, &e.out) < 0) {
                hl_free(&e.out); hl_free(&f.out); goto too_complex;
            }
            hl_free(&e.out);
            PUSH(f);
            break;
        }
        default:
            goto bad_pattern;
        }
    }

    if (stk != 1) goto bad_pattern;
    {
        frag result = stack[0];
        free(stack);
        return result;
    }

too_complex:
    if (errstr) *errstr = "pattern too complex";
    for (int _i = 0; _i < stk; _i++) hl_free(&stack[_i].out);
    free(stack);
    { frag bad; bad.valid = 0; hl_init(&bad.out); return bad; }
bad_pattern:
    if (errstr) *errstr = "invalid pattern";
    for (int _i = 0; _i < stk; _i++) hl_free(&stack[_i].out);
    free(stack);
    { frag bad; bad.valid = 0; hl_init(&bad.out); return bad; }

#undef PUSH
#undef POP
#undef PEEK
}

/* ---- Public entry point --------------------------------------------------- */

int mb_parse(const char *pat, int flags, mb_prog *prog,
             const char **errstr)
{
    if (errstr) *errstr = NULL;

    /* Handle empty pattern: emit MATCH at 0 */
    if (!pat || *pat == '\0') {
        mb_instr in; memset(&in, 0, sizeof(in));
        in.op = I_MATCH;
        if (mb_prog_emit(prog, in) < 0) {
            if (errstr) *errstr = "out of memory";
            return -1;
        }
        return 0;
    }

    /* Step 1: tokenize */
    tokenizer_t tok;
    memset(&tok, 0, sizeof(tok));
    if (tokenize(&tok, pat, flags, errstr) < 0)
        return -1;

    if (tok.n == 0) {
        /* Empty token stream after parsing — emit MATCH */
        mb_instr in; memset(&in, 0, sizeof(in));
        in.op = I_MATCH;
        mb_prog_emit(prog, in);
        return 0;
    }

    /* Step 2: insert explicit concatenation operators */
    token *with_concat = malloc((size_t)(tok.n * 2 + 1) * sizeof(token));
    if (!with_concat) {
        if (errstr) *errstr = "out of memory";
        return -1;
    }
    int n_concat;
    if (insert_concat(tok.buf, tok.n, with_concat, &n_concat) < 0) {
        free(with_concat);
        if (errstr) *errstr = "pattern too complex";
        return -1;
    }

    /* Step 3: infix → postfix */
    token *postfix = malloc((size_t)(n_concat + 1) * sizeof(token));
    if (!postfix) {
        free(with_concat);
        if (errstr) *errstr = "out of memory";
        return -1;
    }
    int n_postfix;
    if (infix_to_postfix(with_concat, n_concat, postfix, &n_postfix, errstr) < 0) {
        free(with_concat);
        free(postfix);
        return -1;
    }
    free(with_concat);

    /* Step 4: build NFA from postfix */
    frag e = build_nfa(postfix, n_postfix, prog, errstr);
    free(postfix);

    if (!e.valid)
        return -1;

    /* Emit MATCH and patch all output holes to it */
    mb_instr match_in; memset(&match_in, 0, sizeof(match_in));
    match_in.op = I_MATCH;
    int match_idx = mb_prog_emit(prog, match_in);
    if (match_idx < 0) {
        if (errstr) *errstr = "pattern too complex";
        return -1;
    }
    hl_patch(prog, &e.out, match_idx);
    hl_free(&e.out);

    /* The entry point is e.start. If e.start != 0, prepend a JUMP.
     * This ensures callers can always start execution at prog->instrs[0]. */
    if (e.start != 0) {
        /* Grow program by 1 at front */
        if (prog->len >= SX_MAX_INSTRS) {
            if (errstr) *errstr = "pattern too complex";
            return -1;
        }
        if (prog->len >= prog->cap) {
            int nc = prog->cap + 1;
            mb_instr *tmp = realloc(prog->instrs, (size_t)nc * sizeof(mb_instr));
            if (!tmp) { if (errstr) *errstr = "out of memory"; return -1; }
            prog->instrs = tmp;
            prog->cap    = nc;
        }
        memmove(&prog->instrs[1], &prog->instrs[0],
                (size_t)prog->len * sizeof(mb_instr));
        prog->len++;

        /* Fix all indices that were >= 0 */
        for (int i = 1; i < prog->len; i++) {
            mb_instr *in = &prog->instrs[i];
            if (in->op == I_SPLIT) {
                if (in->x >= 0) in->x++;
                if (in->y >= 0) in->y++;
            } else if (in->op == I_JUMP) {
                if (in->x >= 0) in->x++;
            } else {
                /* CHAR/CLASS/ANY/BOL/EOL: x is the output (next state) */
                if (in->x >= 0) in->x++;
            }
        }

        /* instrs[0] = JUMP to e.start+1 */
        mb_instr jmp; memset(&jmp, 0, sizeof(jmp));
        jmp.op = I_JUMP;
        jmp.x  = e.start + 1;
        prog->instrs[0] = jmp;
    }

    return 0;
}
