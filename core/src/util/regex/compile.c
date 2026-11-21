/* compile.c — AST construction helpers and AST -> Thompson NFA */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <string.h>

/* ---- AST ------------------------------------------------------------------- */

void mb_ast_init(mb_ast *t)
{
    memset(t, 0, sizeof(*t));
}

void mb_ast_free(mb_ast *t)
{
    free(t->n);
    free(t->sets);
    memset(t, 0, sizeof(*t));
}

int mb_ast_node(mb_ast *t, int type, int a, int b)
{
    if (t->oom) return -1;
    if (t->len >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        ast_node *tmp = realloc(t->n, (size_t)nc * sizeof(ast_node));
        if (!tmp) { t->oom = 1; return -1; }
        t->n = tmp;
        t->cap = nc;
    }
    ast_node *n = &t->n[t->len];
    memset(n, 0, sizeof(*n));
    n->type = (uint8_t)type;
    n->a = a;
    n->b = b;
    n->set = -1;
    return t->len++;
}

int mb_ast_set(mb_ast *t, const mb_charclass *cc)
{
    if (t->oom) return -1;
    /* Reuse an identical set: patterns repeat the same few sets, and
     * fewer distinct sets means fewer byte classes in the DFA. */
    int si = -1;
    for (int i = t->nsets - 1; i >= 0 && i >= t->nsets - 64; i--) {
        if (memcmp(&t->sets[i], cc, sizeof(*cc)) == 0) { si = i; break; }
    }
    if (si < 0) {
        if (t->nsets >= t->setcap) {
            int nc = t->setcap ? t->setcap * 2 : 16;
            mb_charclass *tmp = realloc(t->sets, (size_t)nc * sizeof(mb_charclass));
            if (!tmp) { t->oom = 1; return -1; }
            t->sets = tmp;
            t->setcap = nc;
        }
        t->sets[t->nsets] = *cc;
        si = t->nsets++;
    }
    int n = mb_ast_node(t, A_SET, -1, -1);
    if (n >= 0) t->n[n].set = si;
    return n;
}

int mb_ast_assert(mb_ast *t, int kind)
{
    int n = mb_ast_node(t, A_ASSERT, -1, -1);
    if (n >= 0) t->n[n].kind = (uint8_t)kind;
    return n;
}

int mb_ast_cat(mb_ast *t, int a, int b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    if (t->n[a].type == A_EMPTY) return b;
    if (t->n[b].type == A_EMPTY) return a;
    return mb_ast_node(t, A_CAT, a, b);
}

int mb_ast_alt(mb_ast *t, int a, int b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    return mb_ast_node(t, A_ALT, a, b);
}

/* Children always have smaller indices than their parents, so properties
 * are computed bottom-up in one pass, without recursion. */
int mb_ast_nullable(const mb_ast *t, int root)
{
    if (root < 0) return 1;
    unsigned char *v = malloc((size_t)root + 1);
    if (!v) return 1;
    for (int i = 0; i <= root; i++) {
        const ast_node *n = &t->n[i];
        int r = 1;
        switch (n->type) {
        case A_SET: r = 0; break;
        case A_CAT: r = v[n->a] && v[n->b]; break;
        case A_ALT: r = v[n->a] || v[n->b]; break;
        case A_REP: r = n->min == 0 || v[n->a]; break;
        default:    r = 1; break;
        }
        v[i] = (unsigned char)r;
    }
    int r = v[root];
    free(v);
    return r;
}

int mb_ast_has_assert(const mb_ast *t, int root)
{
    if (root < 0) return 0;
    unsigned char *v = malloc((size_t)root + 1);
    if (!v) return 1;
    for (int i = 0; i <= root; i++) {
        const ast_node *n = &t->n[i];
        int r = 0;
        switch (n->type) {
        case A_ASSERT: case A_BACKREF: r = 1; break;
        case A_CAT: case A_ALT: r = v[n->a] || v[n->b]; break;
        case A_REP: r = v[n->a]; break;
        default: break;
        }
        v[i] = (unsigned char)r;
    }
    int r = v[root];
    free(v);
    return r;
}

/* ---- NFA ------------------------------------------------------------------- */

/*
 * Fragments in the classic Thompson style: a start instruction and a list
 * of dangling exits. The list is threaded through the exits themselves:
 * a hole is (instruction << 1 | use_y), and the hole's field holds the
 * next hole (or -1).
 */
typedef struct { int start; int out; } frag;

static int *hole_field(mb_prog *p, int h)
{
    mb_inst *in = &p->in[h >> 1];
    return (h & 1) ? &in->y : &in->x;
}

static void patch(mb_prog *p, int h, int target)
{
    while (h >= 0) {
        int *f = hole_field(p, h);
        int next = *f;
        *f = target;
        h = next;
    }
}

static int append(mb_prog *p, int l1, int l2)
{
    if (l1 < 0) return l2;
    int h = l1;
    for (;;) {
        int *f = hole_field(p, h);
        if (*f < 0) { *f = l2; break; }
        h = *f;
    }
    return l1;
}

static int emit(mb_prog *p, int op, int kind, int set, int x, int y)
{
    if (p->too_big) return -1;
    if (p->len >= SX_MAX_INSTRS) { p->too_big = 1; return -1; }
    if (p->len >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        mb_inst *tmp = realloc(p->in, (size_t)nc * sizeof(mb_inst));
        if (!tmp) { p->too_big = 1; return -1; }
        p->in = tmp;
        p->cap = nc;
    }
    mb_inst *in = &p->in[p->len];
    in->op = (uint8_t)op;
    in->kind = (uint8_t)kind;
    in->set = set;
    in->x = x;
    in->y = y;
    return p->len++;
}

static frag bad(void) { frag f = { -1, -1 }; return f; }

static frag empty_frag(mb_prog *p)
{
    frag f;
    f.start = emit(p, N_JMP, 0, -1, -1, -1);
    f.out = f.start < 0 ? -1 : f.start << 1;
    return f;
}

static frag cat2(mb_prog *p, frag a, frag b)
{
    if (a.start < 0 || b.start < 0) return bad();
    patch(p, a.out, b.start);
    frag f = { a.start, b.out };
    return f;
}

static frag comp(const mb_ast *t, int i, mb_prog *p);

/* Collect the operands of a chain of CAT (or ALT) nodes, in order. */
static int flatten(const mb_ast *t, int i, int type, int **out, int *nout)
{
    int cap = 16, n = 0, sp = 0, scap = 16;
    int *res = malloc((size_t)cap * sizeof(int));
    int *st = malloc((size_t)scap * sizeof(int));
    if (!res || !st) { free(res); free(st); return -1; }
    st[sp++] = i;
    while (sp > 0) {
        int k = st[--sp];
        if (t->n[k].type == type) {
            if (sp + 2 > scap) {
                scap *= 2;
                int *tmp = realloc(st, (size_t)scap * sizeof(int));
                if (!tmp) { free(res); free(st); return -1; }
                st = tmp;
            }
            st[sp++] = t->n[k].b;
            st[sp++] = t->n[k].a;
        } else {
            if (n >= cap) {
                cap *= 2;
                int *tmp = realloc(res, (size_t)cap * sizeof(int));
                if (!tmp) { free(res); free(st); return -1; }
                res = tmp;
            }
            res[n++] = k;
        }
    }
    free(st);
    *out = res;
    *nout = n;
    return 0;
}

static frag comp_rep(const mb_ast *t, const ast_node *n, mb_prog *p)
{
    int mn = n->min, mx = n->max;
    if (mx == 0) return empty_frag(p);
    frag f = { -1, -1 };
    int have = 0;
    int fixed = mx < 0 ? (mn > 0 ? mn - 1 : 0) : mn;
    for (int k = 0; k < fixed; k++) {
        frag c = comp(t, n->a, p);
        if (c.start < 0) return bad();
        f = have ? cat2(p, f, c) : c;
        have = 1;
    }
    if (mx < 0) {
        frag c = comp(t, n->a, p);
        if (c.start < 0) return bad();
        int s = emit(p, N_SPLIT, 0, -1, c.start, -1);
        if (s < 0) return bad();
        patch(p, c.out, s);
        frag r;
        if (mn == 0) { r.start = s; r.out = (s << 1) | 1; }        /* a* */
        else         { r.start = c.start; r.out = (s << 1) | 1; }  /* a+ */
        return have ? cat2(p, f, r) : r;
    }
    if (mx == mn) return f;
    /* a{mn,mx}: mn copies, then mx-mn nested optional copies */
    int outs = -1, prev_hole = -1;
    int first_split = -1;
    for (int k = mn; k < mx; k++) {
        frag c = comp(t, n->a, p);
        if (c.start < 0) return bad();
        int s = emit(p, N_SPLIT, 0, -1, c.start, -1);
        if (s < 0) return bad();
        if (first_split < 0) first_split = s;
        if (prev_hole >= 0) patch(p, prev_hole, s);
        outs = append(p, outs, (s << 1) | 1);
        prev_hole = c.out;
    }
    outs = append(p, outs, prev_hole);
    frag r = { first_split, outs };
    return have ? cat2(p, f, r) : r;
}

static frag comp(const mb_ast *t, int i, mb_prog *p)
{
    if (i < 0 || p->too_big) return bad();
    const ast_node *n = &t->n[i];
    switch (n->type) {
    case A_EMPTY:
        return empty_frag(p);
    case A_SET: {
        int k = emit(p, N_SET, 0, n->set, -1, -1);
        frag f = { k, k < 0 ? -1 : k << 1 };
        return f;
    }
    case A_ASSERT: {
        p->asserts |= 1 << n->kind;
        int k = emit(p, N_ASSERT, n->kind, -1, -1, -1);
        frag f = { k, k < 0 ? -1 : k << 1 };
        return f;
    }
    case A_CAT: {
        int *ops, nops;
        if (flatten(t, i, A_CAT, &ops, &nops) < 0) { p->too_big = 1; return bad(); }
        frag f = comp(t, ops[0], p);
        for (int k = 1; k < nops && f.start >= 0; k++)
            f = cat2(p, f, comp(t, ops[k], p));
        free(ops);
        return f;
    }
    case A_ALT: {
        int *ops, nops;
        if (flatten(t, i, A_ALT, &ops, &nops) < 0) { p->too_big = 1; return bad(); }
        frag f = comp(t, ops[0], p);
        for (int k = 1; k < nops && f.start >= 0; k++) {
            frag g = comp(t, ops[k], p);
            if (g.start < 0) { f = bad(); break; }
            int s = emit(p, N_SPLIT, 0, -1, f.start, g.start);
            if (s < 0) { f = bad(); break; }
            f.start = s;
            f.out = append(p, f.out, g.out);
        }
        free(ops);
        return f;
    }
    case A_REP:
        return comp_rep(t, n, p);
    default:
        return bad();   /* back-references never reach the NFA */
    }
}

int mb_compile(const mb_ast *t, int root, mb_prog *p)
{
    memset(p, 0, sizeof(*p));
    p->sets  = t->sets;
    p->nsets = t->nsets;
    frag f;
    if (root < 0) {
        /* Nothing to match: one thread on the empty set (set -1), which
         * no byte satisfies, so it dies at once. */
        int m = emit(p, N_MATCH, 0, -1, -1, -1);
        int k = emit(p, N_SET, 0, -1, m, -1);
        if (m < 0 || k < 0) return -1;
        p->start = k;
        return 0;
    }
    f = comp(t, root, p);
    if (f.start < 0 || p->too_big) return -1;
    int m = emit(p, N_MATCH, 0, -1, -1, -1);
    if (m < 0) return -1;
    patch(p, f.out, m);
    p->start = f.start;
    return 0;
}

void mb_prog_free(mb_prog *p)
{
    if (!p) return;
    free(p->in);
    memset(p, 0, sizeof(*p));
}
