/* classify.c — required literals ("musts") for the grep prefilter
 *
 * For every AST node four facts are derived, bottom-up (children always
 * have smaller indices than their parents, so one pass in index order
 * suffices and nothing recurses):
 *
 *   is     the exact set of strings the node matches, if small and finite
 *   left   every match starts with one of these strings
 *   right  every match ends with one of these strings
 *   in     every match contains one of these strings
 *
 * (Zero-width assertions only remove matches, so they are treated as the
 * empty string; that keeps every fact a valid necessary condition.)
 *
 * A literal set L for a pattern means: a line without any string of L
 * cannot match, so lines that do not contain one are skipped without
 * running the DFA. When the patterns are exactly a set of literals with no
 * other syntax ("foo|bar", -F), a literal hit IS a match.
 *
 * With -i the strings are lower-cased and searched case-insensitively.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <string.h>

#define SS_MAX    16    /* strings per set */
#define STR_MAX   32    /* bytes per string */
#define SMALL_SET 4     /* a bracket of up to this many chars is expanded */

typedef struct {
    int    n;               /* -1: unknown (no information) */
    size_t len[SS_MAX];
    char   s[SS_MAX][STR_MAX];
    int    trunc;           /* some string was cut: not an exact set */
} sset;

typedef struct {
    sset is, left, right, in;
} info_t;

static void ss_unknown(sset *a) { a->n = -1; a->trunc = 0; }
static void ss_empty(sset *a)   { a->n = 1; a->len[0] = 0; a->trunc = 0; }

static int ss_has(const sset *a, const char *s, size_t len)
{
    for (int i = 0; i < a->n; i++)
        if (a->len[i] == len && memcmp(a->s[i], s, len) == 0) return 1;
    return 0;
}

/* Add a string; keep_right: if too long keep its tail instead of its head */
static int ss_add(sset *a, const char *s, size_t len, int keep_right)
{
    if (a->n < 0) return -1;
    if (len > STR_MAX) {
        if (keep_right) s += len - STR_MAX;
        len = STR_MAX;
        a->trunc = 1;
    }
    if (ss_has(a, s, len)) return 0;
    if (a->n >= SS_MAX) return -1;
    memcpy(a->s[a->n], s, len);
    a->len[a->n++] = len;
    return 0;
}

/* r = a x b. Returns -1 (r unknown) when too large. */
static int ss_cross(const sset *a, const sset *b, sset *r, int keep_right)
{
    ss_unknown(r);
    if (a->n < 0 || b->n < 0) return -1;
    if (a->n * b->n > SS_MAX) return -1;
    r->n = 0;
    r->trunc = a->trunc || b->trunc;
    for (int i = 0; i < a->n; i++) {
        for (int j = 0; j < b->n; j++) {
            char buf[2 * STR_MAX];
            memcpy(buf, a->s[i], a->len[i]);
            memcpy(buf + a->len[i], b->s[j], b->len[j]);
            if (ss_add(r, buf, a->len[i] + b->len[j], keep_right) < 0) {
                ss_unknown(r);
                return -1;
            }
        }
    }
    return 0;
}

static int ss_union(const sset *a, const sset *b, sset *r)
{
    if (a->n < 0 || b->n < 0) { ss_unknown(r); return -1; }
    sset tmp = *a;
    for (int j = 0; j < b->n; j++)
        if (ss_add(&tmp, b->s[j], b->len[j], 0) < 0) { ss_unknown(r); return -1; }
    tmp.trunc = a->trunc || b->trunc;
    *r = tmp;
    return 0;
}

/* Higher is a better prefilter; <= 0 is useless. */
static int ss_score(const sset *a)
{
    if (a->n <= 0 || a->n > MB_LIT_MAX) return 0;
    size_t mn = (size_t)-1;
    for (int i = 0; i < a->n; i++)
        if (a->len[i] < mn) mn = a->len[i];
    if (mn == 0) return 0;
    if (mn > 16) mn = 16;
    return (int)mn * 10 - (a->n - 1) * 4;
}

static const sset *ss_best(const sset *a, const sset *b)
{
    return ss_score(b) > ss_score(a) ? b : a;
}

/* Strings a byte set stands for: a single char, or a few. */
static void set_strings(const mb_charclass *cc, int icase, sset *r)
{
    char chars[256];
    int n = 0;
    ss_unknown(r);
    for (int c = 0; c < 256; c++) {
        if (!mb_charclass_test(cc, (unsigned char)c)) continue;
        int f = icase ? tolower(c) : c;
        int dup = 0;
        for (int k = 0; k < n; k++) if ((unsigned char)chars[k] == f) { dup = 1; break; }
        if (dup) continue;
        if (n >= SMALL_SET) return;
        chars[n++] = (char)f;
    }
    if (n == 0) return;
    r->n = 0;
    for (int k = 0; k < n; k++) ss_add(r, &chars[k], 1, 0);
}

static void info_unknown(info_t *x)
{
    ss_unknown(&x->is);
    ss_unknown(&x->left);
    ss_unknown(&x->right);
    ss_unknown(&x->in);
}

static void info_empty(info_t *x)
{
    ss_empty(&x->is);
    ss_empty(&x->left);
    ss_empty(&x->right);
    ss_empty(&x->in);
}

/* in := best of in, left, right, is */
static void info_fix_in(info_t *x)
{
    const sset *b = ss_best(&x->in, &x->left);
    b = ss_best(b, &x->right);
    b = ss_best(b, &x->is);
    if (b != &x->in) x->in = *b;
}

static void info_cat(const info_t *a, const info_t *b, info_t *r)
{
    sset tmp;
    /* is */
    if (a->is.n >= 0 && b->is.n >= 0 && ss_cross(&a->is, &b->is, &r->is, 0) == 0)
        ;
    else
        ss_unknown(&r->is);
    /* left */
    if (a->is.n >= 0 && !a->is.trunc) {
        if (ss_cross(&a->is, &b->left, &r->left, 0) < 0) r->left = a->is;
    } else {
        r->left = a->left;
    }
    /* right */
    if (b->is.n >= 0 && !b->is.trunc) {
        if (ss_cross(&a->right, &b->is, &r->right, 1) < 0) r->right = b->is;
    } else {
        r->right = b->right;
    }
    /* in */
    const sset *best = ss_best(&a->in, &b->in);
    if (ss_cross(&a->right, &b->left, &tmp, 0) == 0)
        best = ss_best(best, &tmp);
    r->in = *best;
    if (r->is.n >= 0 && r->is.trunc) {
        /* a truncated exact set still gives true prefixes, not an "is" */
        ss_unknown(&r->is);
    }
    info_fix_in(r);
}

static void info_alt(const info_t *a, const info_t *b, info_t *r)
{
    ss_union(&a->is, &b->is, &r->is);
    ss_union(&a->left, &b->left, &r->left);
    ss_union(&a->right, &b->right, &r->right);
    ss_union(&a->in, &b->in, &r->in);
    info_fix_in(r);
}

static void info_rep(const info_t *a, int mn, int mx, info_t *r)
{
    if (mx == 0) { info_empty(r); return; }
    ss_unknown(&r->is);
    /* exact set of a{mn,mx} for small mx: the union of a^k, mn <= k <= mx */
    if (a->is.n >= 0 && !a->is.trunc && mx > 0 && mx <= 4) {
        sset pw, acc;
        int ok = 1;
        ss_empty(&pw);
        if (mn == 0) ss_empty(&acc);
        else { acc.n = 0; acc.trunc = 0; }
        for (int k = 1; k <= mx; k++) {
            sset nx;
            if (ss_cross(&pw, &a->is, &nx, 0) < 0) { ok = 0; break; }
            pw = nx;
            if (k >= mn && ss_union(&acc, &pw, &acc) < 0) { ok = 0; break; }
        }
        if (ok) r->is = acc;
    }
    if (mn >= 1) {
        r->left = a->left;
        r->right = a->right;
        r->in = a->in;
    } else {
        ss_empty(&r->left);
        ss_empty(&r->right);
        ss_empty(&r->in);
    }
    info_fix_in(r);
}

void mb_litset_free(mb_litset *ls)
{
    for (int i = 0; i < ls->n; i++) free(ls->s[i]);
    memset(ls, 0, sizeof(*ls));
}

/* Rough byte frequencies in text (higher = more common). */
static int freq(unsigned char c)
{
    static const char common[] = " etaoinsrhldcumfpgwybvk";
    if (c == ' ') return 255;
    for (int i = 1; common[i]; i++)
        if ((unsigned char)common[i] == c) return 240 - i * 6;
    if (c >= 'a' && c <= 'z') return 90;
    if (c >= 'A' && c <= 'Z') return 70;
    if (c >= '0' && c <= '9') return (c <= '2') ? 140 : 110;
    if (c == '.' || c == ',' || c == '\t') return 150;
    if (c == '_' || c == '-' || c == '/' || c == ':' || c == '=' ||
        c == '"' || c == '(' || c == ')' || c == ';' || c == '\'') return 100;
    if (c >= 0x21 && c < 0x7f) return 40;
    return 10;
}

static int freq_fold(unsigned char c, int icase)
{
    static short tab[256];
    static int init;
    if (!init) {
        for (int i = 0; i < 256; i++) tab[i] = (short)freq((unsigned char)i);
        init = 1;
    }
    if (icase && isalpha(c))
        return tab[tolower(c)] + tab[toupper(c)];
    return tab[c];
}

void mb_litset_prepare(mb_litset *ls)
{
    ls->maxprobe = 0;
    ls->maxlen = 0;
    for (int i = 0; i < ls->n; i++) {
        if (ls->len[i] > ls->maxlen) ls->maxlen = ls->len[i];
        const unsigned char *s = (const unsigned char *)ls->s[i];
        size_t len = ls->len[i];
        size_t a = 0, b = 0;
        for (size_t k = 1; k < len; k++)
            if (freq_fold(s[k], ls->icase) < freq_fold(s[a], ls->icase)) a = k;
        b = a;
        int bf = 1 << 30;
        for (size_t k = 0; k < len; k++) {
            if (k == a) continue;
            int f = freq_fold(s[k], ls->icase);
            if (f < bf) { bf = f; b = k; }
        }
        if (b < a) { size_t t = a; a = b; b = t; }
        ls->o1[i] = a; ls->o2[i] = b;
        ls->b1[i] = s[a]; ls->b2[i] = s[b];
        ls->f1[i] = (ls->icase && isalpha(s[a])) ? 0x20 : 0;
        ls->f2[i] = (ls->icase && isalpha(s[b])) ? 0x20 : 0;
        if (ls->f1[i]) ls->b1[i] = (uint8_t)tolower(s[a]);
        if (ls->f2[i]) ls->b2[i] = (uint8_t)tolower(s[b]);
        if (b > ls->maxprobe) ls->maxprobe = b;
    }
}

/* Drop strings that contain another string of the set: "contains A or
 * contains AB" is just "contains A". */
static void ss_minimise(sset *a)
{
    for (int i = 0; i < a->n && i < SS_MAX; i++) {
        for (int j = 0; j < a->n && j < SS_MAX; j++) {
            if (i == j || a->len[j] > a->len[i]) continue;
            if (a->len[j] == a->len[i] && j > i) continue;
            int found = 0;
            for (size_t k = 0; k + a->len[j] <= a->len[i]; k++)
                if (memcmp(a->s[i] + k, a->s[j], a->len[j]) == 0) { found = 1; break; }
            if (found) {
                a->n--;
                if (i != a->n) {
                    memcpy(a->s[i], a->s[a->n], a->len[a->n]);
                    a->len[i] = a->len[a->n];
                }
                i = -1;
                break;
            }
        }
    }
}

/* The byte a set stands for when it is a single character (or, with -i,
 * one letter in both cases); -1 otherwise. */
static int set_char1(const mb_charclass *cc, int icase)
{
    uint64_t w[4];
    memcpy(w, cc->bits, sizeof(w));
    int cnt = __builtin_popcountll(w[0]) + __builtin_popcountll(w[1]) +
              __builtin_popcountll(w[2]) + __builtin_popcountll(w[3]);
    if (cnt < 1 || cnt > 2) return -1;
    int c1 = -1, c2 = -1;
    for (int k = 0; k < 4; k++) {
        while (w[k]) {
            int c = k * 64 + __builtin_ctzll(w[k]);
            w[k] &= w[k] - 1;
            if (c1 < 0) c1 = c; else c2 = c;
        }
    }
    if (cnt == 1) {
        if (icase && isalpha(c1)) return -1;   /* -i sets hold both cases */
        return c1;
    }
    /* two: a letter in both cases, with -i */
    if (!icase || tolower(c1) != tolower(c2) || c1 == c2) return -1;
    return tolower(c1);
}

/* If root is a plain string (single characters, and zero-width assertions
 * when allow_assert), return it malloc'ed (*len bytes, NUL-terminated);
 * NULL otherwise. *asserts is set when assertions were skipped. */
char *mb_ast_plain(const mb_ast *t, int root, int icase, size_t *len,
                   int *asserts)
{
    int cap = 64, n = 0, sp = 0, stk[256];
    char *s = malloc((size_t)cap);
    if (!s) return NULL;
    stk[sp++] = root;
    while (sp > 0) {
        int k = stk[--sp];
        const ast_node *x = &t->n[k];
        if (x->type == A_CAT) {
            if (sp + 2 > 256) { free(s); return NULL; }
            stk[sp++] = x->b;
            stk[sp++] = x->a;
            continue;
        }
        if (x->type == A_ASSERT && asserts) {
            /* a leading ^ / trailing $ is reported in bit 1 / bit 2 */
            if (x->kind == AS_BOL && n == 0 && !(*asserts & 4)) *asserts |= 2;
            else if (x->kind == AS_EOL && sp == 0) *asserts |= 4;
            else *asserts |= 1;
            continue;
        }
        int c = x->type == A_SET ? set_char1(&t->sets[x->set], icase) : -1;
        if (c < 0) { free(s); return NULL; }
        if (n + 1 >= cap) {
            cap *= 2;
            char *ns = realloc(s, (size_t)cap);
            if (!ns) { free(s); return NULL; }
            s = ns;
        }
        s[n++] = (char)c;
    }
    s[n] = '\0';
    *len = (size_t)n;
    return s;
}

/*
 * Fast path for the usual case: every pattern is a plain string (a chain
 * of single characters). Then the strings themselves are the literal set,
 * exactly, of any length. Returns 1 if it applied.
 */
static int musts_plain(const mb_ast *t, const int *roots, int nroots,
                       int icase, mb_litset *out)
{
    if (nroots > MB_LIT_MAX) return 0;
    int asserts = 0;
    for (int r = 0; r < nroots; r++) {
        size_t n;
        char *str = mb_ast_plain(t, roots[r], icase, &n, &asserts);
        if (!str) return 0;
        if (n == 0) { free(str); return 0; }
        out->s[out->n] = str;
        out->len[out->n] = n;
        out->n++;
    }
    /* "contains A or contains AB" is "contains A" */
    for (int i = 0; i < out->n && i < SS_MAX; i++) {
        for (int j = 0; j < out->n && j < SS_MAX; j++) {
            if (i == j || out->len[j] > out->len[i] ||
                (out->len[j] == out->len[i] && j > i)) continue;
            int found = 0;
            for (size_t k = 0; k + out->len[j] <= out->len[i]; k++)
                if (memcmp(out->s[i] + k, out->s[j], out->len[j]) == 0) { found = 1; break; }
            if (found) {
                free(out->s[i]);
                out->n--;
                out->s[i] = out->s[out->n];
                out->len[i] = out->len[out->n];
                i = -1;
                break;
            }
        }
    }
    out->exact = !asserts;
    out->icase = icase;
    mb_litset_prepare(out);
    return 1;
}

void mb_musts(const mb_ast *t, const int *roots, int nroots, int icase,
              mb_litset *out)
{
    memset(out, 0, sizeof(*out));
    out->icase = icase;
    if (nroots <= 0) return;
    if (musts_plain(t, roots, nroots, icase, out)) return;
    mb_litset_free(out);
    out->icase = icase;

    int maxroot = -1;
    for (int i = 0; i < nroots; i++) if (roots[i] > maxroot) maxroot = roots[i];
    if (maxroot < 0) return;

    info_t *inf = malloc(((size_t)maxroot + 1) * sizeof(info_t));
    if (!inf) return;
    for (int i = 0; i <= maxroot; i++) {      /* no information until computed */
        ss_unknown(&inf[i].in); ss_unknown(&inf[i].left);
        ss_unknown(&inf[i].right); ss_unknown(&inf[i].is);
    }

    for (int i = 0; i <= maxroot; i++) {
        const ast_node *n = &t->n[i];
        info_t *x = &inf[i];
        switch (n->type) {
        case A_EMPTY:
        case A_ASSERT:
            info_empty(x);
            break;
        case A_SET:
            set_strings(&t->sets[n->set], icase, &x->is);
            x->left = x->right = x->in = x->is;
            break;
        case A_CAT:
            info_cat(&inf[n->a], &inf[n->b], x);
            break;
        case A_ALT:
            info_alt(&inf[n->a], &inf[n->b], x);
            break;
        case A_REP:
            info_rep(&inf[n->a], n->min, n->max, x);
            break;
        default:
            info_unknown(x);
            ss_empty(&x->left);
            ss_empty(&x->right);
            ss_empty(&x->in);
            break;
        }
    }

    /* exact: every pattern is a finite literal set without assertions */
    int exact = 1;
    sset all;
    ss_unknown(&all);
    all.n = 0;
    for (int i = 0; i < nroots && exact; i++) {
        const info_t *x = &inf[roots[i]];
        if (x->is.n < 0 || x->is.trunc || mb_ast_has_assert(t, roots[i]) ||
            ss_union(&all, &x->is, &all) < 0)
            exact = 0;
    }
    if (exact) {
        for (int i = 0; i < all.n; i++) if (all.len[i] == 0) exact = 0;
    }

    sset pick;
    if (exact) {
        pick = all;
    } else {
        ss_unknown(&pick);
        pick.n = 0;
        for (int i = 0; i < nroots; i++) {
            const sset *b = &inf[roots[i]].in;
            if (ss_score(b) <= 0 || ss_union(&pick, b, &pick) < 0) {
                pick.n = 0;
                break;
            }
        }
    }
    free(inf);

    ss_minimise(&pick);
    if (pick.n <= 0 || pick.n > MB_LIT_MAX || ss_score(&pick) <= 0)
        return;

    for (int i = 0; i < pick.n; i++) {
        out->s[i] = malloc(pick.len[i] + 1);
        if (!out->s[i]) { out->n = i; mb_litset_free(out); return; }
        memcpy(out->s[i], pick.s[i], pick.len[i]);
        out->s[i][pick.len[i]] = '\0';
        out->len[i] = pick.len[i];
        out->n = i + 1;
    }
    out->exact = exact;
    out->icase = icase;
    mb_litset_prepare(out);
}
