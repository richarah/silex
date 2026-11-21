/* thompson.c — lazy DFA over the Thompson NFA
 *
 * A DFA state is a set of NFA threads plus the context of the byte before
 * the current position (line start / word byte / other byte). Zero-width
 * assertions that need the NEXT byte too (\b \B \< \> $ and -w's
 * "no word char after") stay in the set unresolved; they are decided when
 * the next byte is seen, as part of that byte's transition. So a match is
 * reported one byte late: the transition on byte i carries a flag meaning
 * "a match ends at i" (and the transition on the end-of-line pseudo byte
 * reports a match ending at the end of the line).
 *
 * Bytes are mapped to equivalence classes (bytes no set and no assertion
 * can tell apart), so a state's transition row has one entry per class plus
 * one for end of line. Rows are filled lazily; when the cache grows past its
 * budget it is flushed and rebuilt from the current state.
 *
 * The '\n' class is special: it ends the line (the end-of-line context
 * holds before it) and, in an unanchored DFA, starts the next one in the
 * line-start state. That lets one DFA pass scan a whole buffer of lines.
 *
 * Unanchored DFAs add the NFA start state at every position (search);
 * anchored ones run the threads started at one position only (match bounds).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <string.h>
#include <stdlib.h>

#define CTX_EOL     3

/*
 * Transition encoding (int32):
 *   >= 0   row offset of the next state; nothing happened
 *   -1     not computed yet
 *   -2     a match ends before this byte, and nothing can follow
 *   -3     dead: no match before this byte, and none can follow
 *   <= -4  a match ends before this byte; continue at row offset -t-4
 * so the scanning loops test a single sign bit per byte.
 */
#define UNK         (-1)
#define T_MDEAD     (-2)
#define T_DEADX     (-3)
#define T_OOM       INT32_MIN
#define IS_MATCH(t) ((t) == T_MDEAD || ((t) <= -4 && (t) != T_OOM))

#define MAX_STATES  10000
#define MAX_POOL    (1 << 22)   /* ints of NFA state sets */

struct mb_dfa {
    const mb_prog *p;
    int      anchored;
    int      use_bol, use_word;
    int      nclass, stride, eol, nl;
    uint8_t  cls[256];
    uint8_t  cword[257];     /* per class: word byte */
    uint8_t  rep[257];       /* a byte of each class */
    uint8_t *acc;            /* acc[set * nclass + class]: set accepts class */
    int      dead_is_empty;

    int      nstates, scap;
    int     *soff, *slen;
    uint8_t *sctx, *sdead;
    int     *pool;
    size_t   plen, pcap;
    int32_t *trans;

    int     *htab;
    int      hsize;
    int      init[3];

    /* scratch */
    int     *stk, *buf1, *buf2, *buf3;
    unsigned *mark;
    unsigned gen;
};

static int norm_ctx(const mb_dfa *d, int c)
{
    if (c == CTX_BOL && !d->use_bol) c = CTX_NONWORD;
    if (c == CTX_WORD && !d->use_word) c = CTX_NONWORD;
    return c;
}

/* 1 holds, 0 fails, -1 needs the next byte */
static int eval(int kind, int prev, int next)
{
    int pw = prev == CTX_WORD;
    int nw = next == CTX_WORD;
    switch (kind) {
    case AS_BOL:      return prev == CTX_BOL;
    case AS_NOTWPREV: return !pw;
    case AS_EOL:      return next < 0 ? -1 : next == CTX_EOL;
    case AS_NOTWNEXT: return next < 0 ? -1 : !nw;
    case AS_WBEG:     if (pw) return 0; return next < 0 ? -1 : nw;
    case AS_WEND:     if (!pw) return 0; return next < 0 ? -1 : !nw;
    case AS_WORDB:    return next < 0 ? -1 : pw != nw;
    case AS_NWORDB:   return next < 0 ? -1 : pw == nw;
    default:          return 0;
    }
}

static void newgen(mb_dfa *d)
{
    if (++d->gen == 0) {
        memset(d->mark, 0, (size_t)d->p->len * sizeof(unsigned));
        d->gen = 1;
    }
}

/* Follow SPLIT/JMP and decidable assertions from in[0..nin). */
static int closure(mb_dfa *d, const int *in, int nin, int extra,
                   int prev, int next, int *out)
{
    const mb_inst *I = d->p->in;
    int sp = 0, n = 0;
    newgen(d);
    for (int i = nin - 1; i >= 0; i--) d->stk[sp++] = in[i];
    if (extra >= 0) d->stk[sp++] = extra;
    while (sp > 0) {
        int pc = d->stk[--sp];
        if (d->mark[pc] == d->gen) continue;
        d->mark[pc] = d->gen;
        const mb_inst *x = &I[pc];
        switch (x->op) {
        case N_SPLIT:
            d->stk[sp++] = x->y;
            d->stk[sp++] = x->x;
            break;
        case N_JMP:
            d->stk[sp++] = x->x;
            break;
        case N_ASSERT: {
            int r = eval(x->kind, prev, next);
            if (r > 0) d->stk[sp++] = x->x;
            else if (r < 0) out[n++] = pc;
            break;
        }
        default:
            out[n++] = pc;
            break;
        }
    }
    return n;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void sort_ints(int *a, int n)
{
    if (n > 24) { qsort(a, (size_t)n, sizeof(int), cmp_int); return; }
    for (int i = 1; i < n; i++) {
        int v = a[i], j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static uint32_t hash_set(const int *s, int n, int ctx)
{
    uint32_t h = 2166136261u ^ (uint32_t)ctx;
    for (int i = 0; i < n; i++) { h ^= (uint32_t)s[i]; h *= 16777619u; }
    return h ^ (uint32_t)n;
}

static void flush(mb_dfa *d)
{
    d->nstates = 0;
    d->plen = 0;
    for (int i = 0; i < d->hsize; i++) d->htab[i] = -1;
    d->init[0] = d->init[1] = d->init[2] = -1;
}

static int grow_states(mb_dfa *d)
{
    int nc = d->scap ? d->scap * 2 : 64;
    if (nc > MAX_STATES) nc = MAX_STATES;
    int *so = realloc(d->soff, (size_t)nc * sizeof(int));
    if (!so) return -1;
    d->soff = so;
    int *sl = realloc(d->slen, (size_t)nc * sizeof(int));
    if (!sl) return -1;
    d->slen = sl;
    uint8_t *sc = realloc(d->sctx, (size_t)nc);
    if (!sc) return -1;
    d->sctx = sc;
    uint8_t *sd = realloc(d->sdead, (size_t)nc);
    if (!sd) return -1;
    d->sdead = sd;
    int32_t *tr = realloc(d->trans, (size_t)nc * (size_t)d->stride * sizeof(int32_t));
    if (!tr) return -1;
    d->trans = tr;
    d->scap = nc;
    return 0;
}

/* Keep the hash table at most half full. */
static int rehash(mb_dfa *d, int want)
{
    int hs = d->hsize;
    while (hs < 2 * want) hs <<= 1;
    if (hs == d->hsize) return 0;
    int *nh = malloc((size_t)hs * sizeof(int));
    if (!nh) return -1;
    for (int i = 0; i < hs; i++) nh[i] = -1;
    for (int st = 0; st < d->nstates; st++) {
        uint32_t h = hash_set(&d->pool[d->soff[st]], d->slen[st], d->sctx[st]);
        int slot = (int)(h & (uint32_t)(hs - 1));
        while (nh[slot] >= 0) slot = (slot + 1) & (hs - 1);
        nh[slot] = st;
    }
    free(d->htab);
    d->htab = nh;
    d->hsize = hs;
    return 0;
}

/* Find or add the state (set, ctx). Sets *flushed when the cache was
 * emptied to make room. Returns the state index, -1 on OOM. */
static int find_or_add(mb_dfa *d, int *set, int n, int ctx, int *flushed)
{
    sort_ints(set, n);
    if (n == 0 && d->dead_is_empty) ctx = 0;
    uint32_t h = hash_set(set, n, ctx);
    int mask = d->hsize - 1;
    int slot = (int)(h & (uint32_t)mask);
    while (d->htab[slot] >= 0) {
        int s = d->htab[slot];
        if (d->slen[s] == n && d->sctx[s] == ctx &&
            (n == 0 || memcmp(&d->pool[d->soff[s]], set, (size_t)n * sizeof(int)) == 0))
            return s;
        slot = (slot + 1) & mask;
    }
    if (d->nstates >= MAX_STATES || d->plen + (size_t)n > MAX_POOL) {
        flush(d);
        *flushed = 1;
        slot = (int)(h & (uint32_t)mask);
    }
    if (d->nstates >= d->scap && grow_states(d) < 0) return -1;
    if (2 * (d->nstates + 1) > d->hsize) {
        if (rehash(d, d->nstates + 1) < 0) return -1;
        mask = d->hsize - 1;
        slot = (int)(h & (uint32_t)mask);
        while (d->htab[slot] >= 0) slot = (slot + 1) & mask;
    }
    if (d->plen + (size_t)n > d->pcap) {
        size_t nc = d->pcap ? d->pcap : 1024;
        while (nc < d->plen + (size_t)n) nc *= 2;
        int *np = realloc(d->pool, nc * sizeof(int));
        if (!np) return -1;
        d->pool = np;
        d->pcap = nc;
    }
    int s = d->nstates++;
    d->soff[s] = (int)d->plen;
    d->slen[s] = n;
    d->sctx[s] = (uint8_t)ctx;
    d->sdead[s] = (uint8_t)(n == 0 && d->dead_is_empty);
    if (n > 0) memcpy(&d->pool[d->plen], set, (size_t)n * sizeof(int));
    d->plen += (size_t)n;
    memset(&d->trans[(size_t)s * (size_t)d->stride], 0xFF,
           (size_t)d->stride * sizeof(int32_t));
    d->htab[slot] = s;
    return s;
}

static int get_init(mb_dfa *d, int ctx, int *flushed)
{
    ctx = norm_ctx(d, ctx);
    if (d->init[ctx] >= 0) return d->init[ctx];
    int n = closure(d, NULL, 0, d->p->start, ctx, -1, d->buf2);
    int s = find_or_add(d, d->buf2, n, ctx, flushed);
    if (s >= 0) d->init[ctx] = s;
    return s;
}

static inline int32_t enc(const mb_dfa *d, int s, int matched, int dead)
{
    if (dead) return matched ? T_MDEAD : T_DEADX;
    int32_t off = (int32_t)s * (int32_t)d->stride;
    return matched ? -off - 4 : off;
}

/* Compute the transition of state s on class k. Returns the encoded
 * transition, or T_OOM on out-of-memory. */
static int32_t compute(mb_dfa *d, int s, int k)
{
    const mb_inst *I = d->p->in;
    int n = d->slen[s];
    if (n > 0) memcpy(d->buf1, &d->pool[d->soff[s]], (size_t)n * sizeof(int));
    int prev = d->sctx[s];
    int next = (k == d->eol || k == d->nl) ? CTX_EOL
             : (d->cword[k] ? CTX_WORD : CTX_NONWORD);

    int nr = closure(d, d->buf1, n, -1, prev, next, d->buf2);
    int matched = 0;
    int nt = 0;
    for (int i = 0; i < nr; i++) {
        const mb_inst *x = &I[d->buf2[i]];
        if (x->op == N_MATCH) matched = 1;
        else if (x->op == N_SET && k != d->eol && k != d->nl && x->set >= 0 &&
                 d->acc[(size_t)x->set * (size_t)d->nclass + (size_t)k])
            d->buf3[nt++] = x->x;
    }

    int flushed = 0, ns;
    if (k == d->eol) {
        int32_t t = matched ? T_MDEAD : T_DEADX;
        d->trans[(size_t)s * (size_t)d->stride + (size_t)k] = t;
        return t;
    }
    if (k == d->nl) {
        if (d->anchored) {
            ns = find_or_add(d, d->buf2, 0, CTX_NONWORD, &flushed);   /* dead */
        } else {
            ns = get_init(d, CTX_BOL, &flushed);
        }
    } else {
        int nctx = norm_ctx(d, d->cword[k] ? CTX_WORD : CTX_NONWORD);
        int m = closure(d, d->buf3, nt, d->anchored ? -1 : d->p->start,
                        nctx, -1, d->buf2);
        ns = find_or_add(d, d->buf2, m, nctx, &flushed);
    }
    if (ns < 0) return T_OOM;
    int32_t t = enc(d, ns, matched, d->sdead[ns]);
    if (!flushed)
        d->trans[(size_t)s * (size_t)d->stride + (size_t)k] = t;
    return t;
}

/* Number the byte classes densely (some may have become empty). */
static int renumber(int *cl)
{
    int ren[512];
    for (int k = 0; k < 512; k++) ren[k] = -1;
    int nn = 0;
    for (int c = 0; c < 256; c++) {
        if (ren[cl[c]] < 0) ren[cl[c]] = nn++;
        cl[c] = ren[cl[c]];
    }
    return nn;
}

mb_dfa *mb_dfa_new(const mb_prog *p, int anchored)
{
    mb_dfa *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->p = p;
    d->anchored = anchored;
    d->use_bol  = (p->asserts & (1 << AS_BOL)) != 0;
    d->use_word = (p->asserts & ((1 << AS_WORDB) | (1 << AS_NWORDB) |
                                 (1 << AS_WBEG) | (1 << AS_WEND) |
                                 (1 << AS_NOTWPREV) | (1 << AS_NOTWNEXT))) != 0;
    /* NOTWPREV and the word assertions treat line start as non-word, so
     * they need no separate BOL context. */

    /* Byte classes: refine by every set used, by word-ness, and keep '\n'
     * on its own. */
    unsigned char *used = calloc((size_t)(p->nsets ? p->nsets : 1), 1);
    if (!used) { free(d); return NULL; }
    for (int i = 0; i < p->len; i++)
        if (p->in[i].op == N_SET && p->in[i].set >= 0) used[p->in[i].set] = 1;
    /* Refine a partition of the bytes by each predicate: relabel the
     * bytes on the smaller side of the split with fresh class numbers
     * (one per old class), then renumber densely. */
    int cl[256];
    for (int c = 0; c < 256; c++) cl[c] = 0;
    int ncl = 1;
    mb_charclass pred;
    for (int step = -2; step < p->nsets; step++) {
        if (step == -2) {
            memset(&pred, 0, sizeof(pred));
            mb_charclass_set(&pred, '\n');
        } else if (step == -1) {
            if (!d->use_word) continue;
            static const uint8_t word_bits[32] = {
                0, 0, 0, 0, 0, 0, 0xFF, 0x03,          /* 0-9 */
                0xFE, 0xFF, 0xFF, 0x87,                /* A-Z _ */
                0xFE, 0xFF, 0xFF, 0x07                 /* a-z */
            };
            memcpy(pred.bits, word_bits, 32);
        } else {
            if (!used[step]) continue;
            pred = p->sets[step];
        }
        int cnt = 0;
        for (int w = 0; w < 32; w++) cnt += __builtin_popcount(pred.bits[w]);
        if (cnt == 0 || cnt == 256) continue;
        int flip = cnt > 128 ? 0xFF : 0;
        int map[512];
        for (int k = 0; k < ncl; k++) map[k] = -1;
        for (int w = 0; w < 32; w++) {
            unsigned bits = (unsigned)(pred.bits[w] ^ flip) & 0xFFu;
            while (bits) {
                int c = w * 8 + __builtin_ctz(bits);
                bits &= bits - 1;
                int o = cl[c];
                if (map[o] < 0) map[o] = ncl++;
                cl[c] = map[o];
            }
        }
        if (ncl >= 256) ncl = renumber(cl);
    }
    ncl = renumber(cl);
    free(used);
    d->nclass = ncl;
    d->eol = ncl;
    d->stride = ncl + 1;
    for (int c = 255; c >= 0; c--) {
        d->cls[c] = (uint8_t)cl[c];
        d->rep[cl[c]] = (uint8_t)c;
    }
    for (int k = 0; k < ncl; k++) d->cword[k] = (uint8_t)mb_is_word(d->rep[k]);
    d->nl = cl['\n'];
    d->acc = malloc((size_t)(p->nsets ? p->nsets : 1) * (size_t)ncl);
    if (!d->acc) { free(d); return NULL; }
    for (int si = 0; si < p->nsets; si++)
        for (int k = 0; k < ncl; k++)
            d->acc[(size_t)si * (size_t)ncl + (size_t)k] =
                (uint8_t)(k != d->nl && mb_charclass_test(&p->sets[si], d->rep[k]));

    size_t L = (size_t)p->len + 1;
    d->stk  = malloc((3 * L + 8) * sizeof(int));
    d->buf1 = malloc(L * sizeof(int));
    d->buf2 = malloc(L * sizeof(int));
    d->buf3 = malloc(L * sizeof(int));
    d->mark = calloc(L, sizeof(unsigned));
    d->hsize = 64;          /* grows with the state count (rehash) */
    d->htab = malloc((size_t)d->hsize * sizeof(int));
    if (!d->stk || !d->buf1 || !d->buf2 || !d->buf3 || !d->mark || !d->htab) {
        mb_dfa_free(d);
        return NULL;
    }
    flush(d);

    /* An empty thread set is final when no new thread can start later in
     * the line: always when anchored, and when the start state cannot
     * get past its assertions anywhere but at a line start. */
    if (anchored) {
        d->dead_is_empty = 1;
    } else {
        int a = closure(d, NULL, 0, p->start, CTX_NONWORD, -1, d->buf2);
        int b = d->use_word ? closure(d, NULL, 0, p->start, CTX_WORD, -1, d->buf2) : 0;
        d->dead_is_empty = (a == 0 && b == 0);
    }
    return d;
}

void mb_dfa_free(mb_dfa *d)
{
    if (!d) return;
    free(d->acc);
    free(d->soff); free(d->slen); free(d->sctx); free(d->sdead);
    free(d->pool); free(d->trans); free(d->htab);
    free(d->stk); free(d->buf1); free(d->buf2); free(d->buf3); free(d->mark);
    free(d);
}

/* Transition lookup, computing it on a miss. st is the state's row offset. */
#define STEP(d, st, k, t) do { \
    (t) = (d)->trans[(size_t)(st) + (size_t)(k)]; \
    if (unlikely((t) == UNK)) \
        (t) = compute((d), (int)((size_t)(st) / (size_t)(d)->stride), (k)); \
} while (0)

static int init_off(mb_dfa *d, int ctx, size_t *st)
{
    int fl = 0;
    int is = get_init(d, ctx, &fl);
    if (is < 0) return -1;
    *st = (size_t)is * (size_t)d->stride;
    return 0;
}

HOT int mb_dfa_line(mb_dfa *d, const unsigned char *s, size_t n, int prev)
{
    size_t st;
    if (!d) return 0;
    if (init_off(d, prev, &st) < 0) return 0;
    const uint8_t *cls = d->cls;
    for (size_t i = 0; i < n; i++) {
        int32_t t = d->trans[st + cls[s[i]]];
        if (likely(t >= 0)) { st = (size_t)t; continue; }
        if (t == UNK) {
            t = compute(d, (int)(st / (size_t)d->stride), cls[s[i]]);
            if (t >= 0) { st = (size_t)t; continue; }
        }
        if (t == T_OOM) return 0;
        if (t != T_DEADX) return 1;
        /* dead: nothing can match before the next line */
        const unsigned char *nl = memchr(s + i, '\n', n - i);
        if (!nl) return 0;
        i = (size_t)(nl - s);
        if (init_off(d, CTX_BOL, &st) < 0) return 0;
    }
    int32_t t;
    STEP(d, st, d->eol, t);
    return IS_MATCH(t);
}

/* One scanning chain: advance over buf[*pi..end) until a match or the end.
 * Returns 1 on a match (at *pi), 0 at the end, -1 on error. */
static inline int chain_slow(mb_dfa *d, const unsigned char *buf, size_t end,
                             size_t *pi, size_t *pst, int32_t t)
{
    size_t i = *pi;
    if (t == UNK) {
        t = compute(d, (int)(*pst / (size_t)d->stride), d->cls[buf[i]]);
        if (t >= 0) { *pst = (size_t)t; *pi = i + 1; return 2; }
    }
    if (t == T_OOM) return -1;
    if (t != T_DEADX) return 1;
    const unsigned char *nl = memchr(buf + i, '\n', end - i);
    if (!nl) { *pi = end; return 3; }   /* dead up to the end */
    *pi = (size_t)(nl - buf) + 1;
    if (init_off(d, CTX_BOL, pst) < 0) return -1;
    return 2;
}

/* Scan buf[i..end) from state st for a match. 1: match at *pi; 0: none
 * before end (*pst is the state at end); 3: none, and the line still open
 * at end cannot match either; -1: error. */
static int chain_run(mb_dfa *d, const unsigned char *buf, size_t end,
                     size_t *pi, size_t *pst)
{
    const uint8_t *cls = d->cls;
    size_t i = *pi, st = *pst;
    size_t ist = (size_t)-1;
    {
        int fl = 0;
        int s0 = d->init[norm_ctx(d, CTX_NONWORD)];
        if (s0 < 0) s0 = get_init(d, CTX_NONWORD, &fl);
        if (s0 >= 0) ist = (size_t)s0 * (size_t)d->stride;
    }
    for (;;) {
        const int32_t *T = d->trans;
        /* In the start state most bytes lead back to it: skip them with
         * loads that do not depend on each other. */
        if (st == ist) {
            const int32_t self = (int32_t)ist;
            while (i + 4 <= end &&
                   T[ist + cls[buf[i]]] == self && T[ist + cls[buf[i + 1]]] == self &&
                   T[ist + cls[buf[i + 2]]] == self && T[ist + cls[buf[i + 3]]] == self)
                i += 4;
        }
        while (i + 4 <= end) {
            int32_t t0 = T[st + cls[buf[i]]];
            if (unlikely(t0 < 0)) goto slow;
            st = (size_t)t0;
            int32_t t1 = T[st + cls[buf[i + 1]]];
            if (unlikely(t1 < 0)) { i += 1; goto slow; }
            st = (size_t)t1;
            int32_t t2 = T[st + cls[buf[i + 2]]];
            if (unlikely(t2 < 0)) { i += 2; goto slow; }
            st = (size_t)t2;
            int32_t t3 = T[st + cls[buf[i + 3]]];
            if (unlikely(t3 < 0)) { i += 3; goto slow; }
            st = (size_t)t3;
            i += 4;
            if (st == ist) break;
        }
        if (st == ist && i + 4 <= end) continue;
        if (i >= end) break;
        {
            int32_t t = T[st + cls[buf[i]]];
            if (t >= 0) { st = (size_t)t; i++; continue; }
        }
    slow: {
            int32_t t = d->trans[st + cls[buf[i]]];
            int r = chain_slow(d, buf, end, &i, &st, t);
            if (r != 2) { *pi = i; *pst = st; return r; }
            /* the cache may have been flushed: the start state moved */
            int s0 = d->init[norm_ctx(d, CTX_NONWORD)];
            ist = s0 >= 0 ? (size_t)s0 * (size_t)d->stride : (size_t)-1;
        }
    }
    *pi = i;
    *pst = st;
    return 0;
}

HOT int mb_dfa_scan(mb_dfa *d, const unsigned char *buf, size_t n, size_t *hit)
{
    size_t st, i = 0;
    if (!d) return 0;
    if (init_off(d, CTX_BOL, &st) < 0) return 0;
    int r = chain_run(d, buf, n, &i, &st);
    if (r < 0 || r == 3) return 0;
    if (r == 1) { *hit = i; return 1; }
    if (n > 0 && buf[n - 1] != '\n') {
        int32_t t;
        STEP(d, st, d->eol, t);
        if (IS_MATCH(t)) { *hit = n; return 1; }
    }
    return 0;
}

/* Does every line match, the empty one included? (The line-start state
 * reports a match before any byte and at the end of the line.) */
int mb_dfa_all_lines(mb_dfa *d)
{
    size_t st;
    if (init_off(d, CTX_BOL, &st) < 0) return 0;
    for (int k = 0; k <= d->eol; k++) {
        int32_t t;
        STEP(d, st, k, t);
        if (!IS_MATCH(t)) return 0;
        if (init_off(d, CTX_BOL, &st) < 0) return 0;   /* in case of a flush */
    }
    return 1;
}

long mb_dfa_first_end(mb_dfa *d, const unsigned char *s, size_t n, size_t from)
{
    size_t st;
    if (!d) return -1;
    if (init_off(d, mb_ctx_at(s, from), &st) < 0) return -1;
    for (size_t i = from; i < n; i++) {
        int32_t t;
        STEP(d, st, d->cls[s[i]], t);
        if (t >= 0) { st = (size_t)t; continue; }
        if (IS_MATCH(t)) return (long)i;
        return -1;   /* dead or out of memory */
    }
    int32_t t;
    STEP(d, st, d->eol, t);
    return IS_MATCH(t) ? (long)n : -1;
}

long mb_dfa_anchored(mb_dfa *d, const unsigned char *s, size_t n, size_t from,
                     size_t *ends, size_t *nends)
{
    long best = -1;
    size_t cnt = 0, st, i;
    if (nends) *nends = 0;
    if (init_off(d, mb_ctx_at(s, from), &st) < 0) return -1;
    for (i = from; i < n; i++) {
        int32_t t;
        STEP(d, st, d->cls[s[i]], t);
        if (t >= 0) { st = (size_t)t; continue; }
        if (!IS_MATCH(t)) break;
        best = (long)i;
        if (ends) ends[cnt++] = i;
        if (t == T_MDEAD) break;
        st = (size_t)(-(t + 4));
    }
    if (i == n) {
        int32_t t;
        STEP(d, st, d->eol, t);
        if (IS_MATCH(t)) { best = (long)n; if (ends) ends[cnt++] = n; }
    }
    if (nends) *nends = cnt;
    return best;
}
