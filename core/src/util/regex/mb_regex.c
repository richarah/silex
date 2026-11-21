/* mb_regex.c — mb_regex public API: pattern sets, line scanning, match
 * bounds, and the libc fallback for back-references */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define E_ESIZE  "Regular expression too big"
#define E_ESPACE "Memory exhausted"

/* ---- compile -------------------------------------------------------------- */

typedef struct {
    const char *msg[64];
    int n;
} warnlist;

/* Compile the NFA programs (once). */
static int progs(mb_regex *re)
{
    if (re->progs_state) return re->progs_state > 0 ? 0 : -1;
    re->progs_state = -1;
    if (mb_compile(&re->ast, re->in_root, &re->in_prog) < 0) return -1;
    if (!re->sel_is_in && mb_compile(&re->ast, re->sel_root, &re->sel_prog) < 0)
        return -1;
    re->progs_state = 1;
    return 0;
}

static void collect_warn(void *ctx, const char *msg)
{
    warnlist *w = ctx;
    if (w->n < 64) w->msg[w->n++] = msg;
}

/* A minimal growable byte string */
typedef struct { char *s; size_t n, cap; } strbuf_lite;

static int sl_add(strbuf_lite *b, const char *p, size_t n, int nl_first)
{
    size_t need = b->n + n + 2;
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < need) nc *= 2;
        char *t = realloc(b->s, nc);
        if (!t) return -1;
        b->s = t;
        b->cap = nc;
    }
    if (nl_first) b->s[b->n++] = '\n';
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = '\0';
    return 0;
}

static int posix_add(mb_regex *re, const char *pat, size_t len, int flags,
                     const mb_parsed *pr, const char **errstr)
{
    mb_posix *np = realloc(re->posix, (size_t)(re->nposix + 1) * sizeof(mb_posix));
    if (!np) { *errstr = E_ESPACE; return -1; }
    re->posix = np;
    char *tmp = malloc(2 * len + 1);
    if (!tmp) { *errstr = E_ESPACE; return -1; }
    size_t o = 0;
    int e = 0;
    for (size_t i = 0; i < len; i++) {
        while (e < pr->nedits && pr->edit_pos[e] < i) e++;
        if (e < pr->nedits && pr->edit_pos[e] == i) {
            if (pr->edit_kind[e] == 'd') continue;
            tmp[o++] = '\\';
        }
        tmp[o++] = pat[i];
    }
    tmp[o] = '\0';
    int rflags = 0;
    if (flags & SX_REG_ERE)   rflags |= REG_EXTENDED;
    if (flags & SX_REG_ICASE) rflags |= REG_ICASE;
    mb_posix *p = &re->posix[re->nposix];
    int rc = regcomp(&p->re, tmp, rflags);
    free(tmp);
    if (rc != 0) {
        static char ebuf[256];
        regerror(rc, &p->re, ebuf, sizeof(ebuf));
        *errstr = ebuf;
        return -1;
    }
    p->ok = 1;
    re->nposix++;
    return 0;
}

mb_regex *mb_regex_compile_set(const char *const *pats, int npats, int flags,
                               const char **errstr, int *errpat,
                               void (*warn)(const char *msg))
{
    const char *dummy;
    int dpat;
    if (!errstr) errstr = &dummy;
    if (!errpat) errpat = &dpat;
    *errstr = NULL;
    *errpat = -1;

    mb_regex *re = calloc(1, sizeof(*re));
    if (!re) { *errstr = E_ESPACE; return NULL; }
    re->flags = flags;
    mb_ast_init(&re->ast);

    int icase = (flags & SX_REG_ICASE) != 0;
    /* per-pattern arrays: on the stack for the usual few patterns */
    enum { SMALL = 16 };
    int d_sb[SMALL], g_sb[SMALL], a_sb[SMALL];
    const char *t_sb[SMALL];
    size_t l_sb[SMALL];
    int cap = SMALL, nd = 0, na = 0, any_rparen = 0, heap = 0;
    int *droots = d_sb;           /* DFA patterns */
    int *groots = g_sb;           /* same, glibc's reading */
    int *aroots = a_sb;           /* back-reference patterns + the selector */
    const char **dtext = t_sb;    /* DFA pattern texts */
    size_t *dlen = l_sb;
    warnlist wl;
    wl.n = 0;
    int nsub = 0;

    for (int i = 0; i < npats; i++) {
        const char *p = pats[i];
        for (;;) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (nd + 1 >= cap || na + 1 >= cap) {
                int nc = cap * 2;
                int *a1 = malloc((size_t)nc * sizeof(int));
                int *a2 = malloc((size_t)nc * sizeof(int));
                int *a3 = malloc((size_t)nc * sizeof(int));
                const char **a4 = malloc((size_t)nc * sizeof(char *));
                size_t *a5 = malloc((size_t)nc * sizeof(size_t));
                if (!a1 || !a2 || !a3 || !a4 || !a5) {
                    free(a1); free(a2); free(a3); free(a4); free(a5);
                    goto oom;
                }
                memcpy(a1, droots, (size_t)nd * sizeof(int));
                memcpy(a2, groots, (size_t)nd * sizeof(int));
                memcpy(a3, aroots, (size_t)na * sizeof(int));
                memcpy(a4, dtext, (size_t)nd * sizeof(char *));
                memcpy(a5, dlen, (size_t)nd * sizeof(size_t));
                if (heap) { free(droots); free(groots); free(aroots); free(dtext); free(dlen); }
                droots = a1; groots = a2; aroots = a3; dtext = a4; dlen = a5;
                heap = 1;
                cap = nc;
            }
            nsub++;
            if (flags & SX_REG_FIXED) {
                int r = mb_parse_fixed(&re->ast, p, len, icase);
                if (r < 0) goto oom;
                groots[nd] = r;
                droots[nd++] = r;
            } else {
                mb_parsed pr;
                const char *err = NULL;
                if (mb_parse(&re->ast, p, len, flags, &pr, &err,
                             collect_warn, &wl) < 0) {
                    *errstr = err;
                    *errpat = i;
                    goto fail;
                }
                if (pr.has_backref) {
                    if (posix_add(re, p, len, flags, &pr, errstr) < 0) {
                        *errpat = i;
                        goto fail;
                    }
                } else {
                    int groot = pr.root;
                    if (pr.quirk) {
                        /* GNU grep has glibc check every pattern: its
                         * reading must be valid too */
                        mb_parsed pg;
                        const char *e2 = NULL;
                        if (mb_parse(&re->ast, p, len, flags | SX_P_GLIBC, &pg,
                                     &e2, NULL, NULL) < 0) {
                            *errstr = e2;
                            *errpat = i;
                            goto fail;
                        }
                        groot = pg.root;
                        re->quirk = 1;
                    }
                    if (pr.rparen_lit && (flags & (SX_REG_WORD | SX_REG_LINE)))
                        any_rparen = 1;
                    groots[nd] = groot;
                    dtext[nd] = p;
                    dlen[nd] = len;
                    droots[nd++] = pr.root;
                }
                if (pr.has_backref) aroots[na++] = pr.root;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    re->nothing = (nsub == 0);

    if (nd > 0) {
        int in_root = -1, d_root = -1;
        /* With a back-reference anywhere GNU grep cannot trust its DFA
         * and checks every line with glibc's regex: glibc's reading then
         * selects lines too. */
        int dfa_reading = re->quirk && re->nposix == 0;
        for (int k = 0; k < nd; k++) {
            in_root = mb_ast_alt(&re->ast, in_root, groots[k]);
            d_root  = dfa_reading ? mb_ast_alt(&re->ast, d_root, droots[k]) : in_root;
        }
        int sel_root = d_root;
        /* (Not when every pattern is a plain string: GNU grep then
         * switches to its fixed-string matcher, which has no wrapper.) */
        int all_plain = re->nposix == 0;
        for (int k = 0; k < nd && all_plain && any_rparen; k++) {
            size_t l;
            char *str = mb_ast_plain(&re->ast, droots[k], icase, &l, NULL);
            if (!str) all_plain = 0;
            free(str);
        }
        if ((flags & (SX_REG_LINE | SX_REG_WORD)) && any_rparen && !all_plain) {
            /* GNU grep selects -x/-w lines with the patterns pasted into
             * "^(...)$" or "(^|\W)(...)(\W|$)" as TEXT, so an unmatched
             * ')' (an ordinary character on its own) closes the wrapper's
             * group. Do the same. */
            const char *pre = (flags & SX_REG_LINE) ? "^(" : "(^|[^[:alnum:]_])(";
            const char *suf = (flags & SX_REG_LINE) ? ")$" : ")([^[:alnum:]_]|$)";
            strbuf_lite w = { NULL, 0, 0 };
            mb_parsed pw;
            const char *e2 = NULL;
            int bad = sl_add(&w, pre, strlen(pre), 0) < 0;
            for (int k = 0; k < nd && !bad; k++)
                bad = sl_add(&w, dtext[k], dlen[k], k > 0) < 0;
            if (bad || sl_add(&w, suf, strlen(suf), 0) < 0) { free(w.s); goto oom; }
            int ok = mb_parse(&re->ast, w.s, w.n, flags | SX_P_NLALT, &pw,
                              &e2, NULL, NULL) == 0;
            free(w.s);
            if (ok) {
                sel_root = pw.root;
                re->quirk = 1;
                flags &= ~(SX_REG_LINE | SX_REG_WORD);   /* already wrapped */
            }
        }
        if (flags & SX_REG_LINE) {
            sel_root = mb_ast_cat(&re->ast, mb_ast_assert(&re->ast, AS_BOL), d_root);
            sel_root = mb_ast_cat(&re->ast, sel_root, mb_ast_assert(&re->ast, AS_EOL));
        } else if (flags & SX_REG_WORD) {
            sel_root = mb_ast_cat(&re->ast, mb_ast_assert(&re->ast, AS_NOTWPREV), d_root);
            sel_root = mb_ast_cat(&re->ast, sel_root, mb_ast_assert(&re->ast, AS_NOTWNEXT));
        }
        if (re->ast.oom) goto oom;
        re->in_root  = in_root;
        re->sel_root = sel_root;
        re->sel_is_in = (sel_root == in_root);
        /* literals are required by the selecting program: the textual
         * -w/-x wrapping above can change what the patterns mean */
        aroots[na++] = sel_root;
        /* the programs and DFAs are built on first use: a literal pattern
         * may never need one, and -o/--color are the only users of the
         * match-bounds DFAs */
        re->have_dfa = 1;
    }

    /* Plain strings, possibly anchored (^foo, foo$): with -w, -x or an
     * anchor the literal hits are checked directly, without a DFA. */
    if (re->nposix == 0 && nd > 0 && nd <= MB_LIT_MAX && !re->quirk &&
        ((re->flags & (SX_REG_WORD | SX_REG_LINE)) || !(flags & SX_REG_FIXED))) {
        int anchors = 0;
        re->plain_ok = 1;
        re->plain.icase = icase;
        for (int k = 0; k < nd && re->plain_ok; k++) {
            size_t len;
            int as = 0;
            char *str = mb_ast_plain(&re->ast, droots[k], icase, &len, &as);
            if (!str || len == 0 || (as & 1)) { free(str); re->plain_ok = 0; break; }
            re->pbol[re->plain.n] = (as & 2) != 0;
            re->peol[re->plain.n] = (as & 4) != 0;
            anchors |= as;
            re->plain.s[re->plain.n] = str;
            re->plain.len[re->plain.n] = len;
            re->plain.n++;
        }
        if (re->plain_ok && (re->flags & SX_REG_WORD) && anchors) re->plain_ok = 0;
        if (re->plain_ok && !(re->flags & (SX_REG_WORD | SX_REG_LINE)) && !anchors)
            re->plain_ok = 0;            /* the exact literal search covers it */
        if (re->plain_ok) mb_litset_prepare(&re->plain);
        else mb_litset_free(&re->plain);
    }
    if (re->ast.len < 200000)
        mb_musts(&re->ast, aroots, na, icase, &re->lits);
    if (re->nposix || (re->flags & (SX_REG_WORD | SX_REG_LINE)))
        re->lits.exact = 0;
    if (re->have_dfa && !(re->lits.exact || re->plain_ok) && progs(re) < 0) {
        *errstr = E_ESIZE; *errpat = 0; goto fail;
    }
    re->pf_on = re->lits.n > 0;
    /* Scanning a buffer, the DFA itself skips bytes that cannot start a
     * match about as fast as a search for one required byte would, and
     * does not run twice over the lines that have it. */
    re->pf_scan = re->pf_on && (re->lits.exact || re->lits.maxlen > 1);
    if (re->lits.n == 1 && re->lits.len[0] > 0 &&
        !(icase && isalpha((unsigned char)re->lits.s[0][0])))
        re->first_char = (unsigned char)re->lits.s[0][0];

    if (heap) { free(droots); free(groots); free(aroots); free(dtext); free(dlen); }
    if (warn)
        for (int k = 0; k < wl.n; k++) warn(wl.msg[k]);
    return re;

oom:
    *errstr = E_ESPACE;
    if (*errpat < 0) *errpat = 0;
fail:
    if (heap) { free(droots); free(groots); free(aroots); free(dtext); free(dlen); }
    mb_regex_free(re);
    return NULL;
}

mb_regex *mb_regex_compile(const char *pat, int flags, const char **errstr)
{
    const char *p = pat ? pat : "";
    return mb_regex_compile_set(&p, 1, flags & ~(SX_REG_WORD | SX_REG_LINE),
                                errstr, NULL, NULL);
}

void mb_regex_free(mb_regex *re)
{
    if (!re) return;
    mb_dfa_free(re->sel);
    mb_dfa_free(re->in_un);
    mb_dfa_free(re->in_an);
    mb_prog_free(&re->in_prog);
    mb_prog_free(&re->sel_prog);
    for (int i = 0; i < re->nposix; i++)
        if (re->posix[i].ok) regfree(&re->posix[i].re);
    free(re->posix);
    mb_litset_free(&re->lits);
    mb_litset_free(&re->plain);
    mb_ast_free(&re->ast);
    free(re->ends);
    free(re);
}

unsigned char mb_regex_first_char(const mb_regex *re)
{
    return re ? (unsigned char)re->first_char : 0;
}

/* The line-selecting DFA, built on first use. NULL on out-of-memory. */
static mb_dfa *sel_dfa(mb_regex *re)
{
    if (!re->sel && progs(re) < 0) return NULL;
    if (!re->sel)
        re->sel = mb_dfa_new(re->sel_is_in ? &re->in_prog : &re->sel_prog, 0);
    return re->sel;
}

int mb_regex_all_lines(mb_regex *re)
{
    if (!re || re->nothing || !re->have_dfa) return 0;
    if (re->lits.n > 0) return 0;     /* a required literal: not "" */
    mb_dfa *d = sel_dfa(re);
    return d ? mb_dfa_all_lines(d) : 0;
}

size_t mb_regex_count_nl(const char *p, size_t n)
{
    return mb_count_nl(p, n);
}

/* ---- libc patterns --------------------------------------------------------- */

static char  *g_tmp;
static size_t g_tmpcap;

/* NUL-terminated copy of s[0..n) for regexec() */
static const char *cstr(const char *s, size_t n)
{
    if (n + 1 > g_tmpcap) {
        size_t nc = g_tmpcap ? g_tmpcap : 256;
        while (nc < n + 1) nc *= 2;
        char *t = realloc(g_tmp, nc);
        if (!t) return NULL;
        g_tmp = t;
        g_tmpcap = nc;
    }
    memcpy(g_tmp, s, n);
    g_tmp[n] = '\0';
    return g_tmp;
}

/* Leftmost-longest match of libc pattern k in s[from..n); offsets. The
 * text before `from` is context (for ^, \< and \b) where the C library
 * can be told about it (REG_STARTEND). noteol: n is not the line end. */
static int posix_search(const mb_regex *re, int k, const char *s, size_t n,
                        size_t from, size_t *ms, size_t *me, int noteol)
{
    const char *c = cstr(s, n);
    if (!c) return 0;
    regmatch_t pm;
    int ef = noteol ? REG_NOTEOL : 0;
#ifdef REG_STARTEND
    pm.rm_so = (regoff_t)from;
    pm.rm_eo = (regoff_t)n;
    if (regexec(&re->posix[k].re, c, 1, &pm, ef | REG_STARTEND) != 0) return 0;
    *ms = (size_t)pm.rm_so;
    *me = (size_t)pm.rm_eo;
#else
    if (regexec(&re->posix[k].re, c + from, 1, &pm,
                ef | (from > 0 ? REG_NOTBOL : 0)) != 0) return 0;
    *ms = from + (size_t)pm.rm_so;
    *me = from + (size_t)pm.rm_eo;
#endif
    return 1;
}

static int is_wordc(const char *s, size_t n, size_t i)
{
    return i < n && mb_is_word((unsigned char)s[i]);
}

/* Next acceptable match of libc pattern k, honouring -w / -x, with the
 * same retry rules as the DFA side (dfa_next_word). */
static int posix_next(const mb_regex *re, int k, const char *s, size_t n,
                      size_t from, size_t *ms, size_t *me)
{
    if (re->flags & SX_REG_LINE) {
        if (from > 0) return 0;
        size_t a, b;
        if (!posix_search(re, k, s, n, 0, &a, &b, 0) || a != 0 || b != n) return 0;
        *ms = 0; *me = n;
        return 1;
    }
    size_t f = from;
    while (f <= n) {
        size_t a, b;
        if (!posix_search(re, k, s, n, f, &a, &b, 0)) return 0;
        if (!(re->flags & SX_REG_WORD)) { *ms = a; *me = b; return 1; }
        if (a == 0 || !mb_is_word((unsigned char)s[a - 1])) {
            size_t cur = b;
            for (;;) {
                if (!is_wordc(s, n, cur)) { *ms = a; *me = cur; return 1; }
                if (cur == a || cur < 1 + from) break;
                size_t lim = cur - 1 - from, x, y;
                if (lim < a || !posix_search(re, k, s, lim, a, &x, &y, 1) ||
                    x != a || y == a)
                    break;
                cur = y;
            }
        }
        f = a + 1;
    }
    return 0;
}

static int posix_line_any(const mb_regex *re, const char *s, size_t n)
{
    for (int k = 0; k < re->nposix; k++) {
        size_t a, b;
        if (posix_next(re, k, s, n, 0, &a, &b)) return 1;
    }
    return 0;
}

/* ---- matching ---------------------------------------------------------------- */

static int lit_eq(const char *a, const char *b, size_t n, int icase)
{
    if (!icase) return memcmp(a, b, n) == 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != (unsigned char)b[i]) return 0;
    return 1;
}

/* -w / -x with plain-string patterns: check the literal occurrences */
static int plain_line(const mb_regex *re, const char *s, size_t n)
{
    const mb_litset *L = &re->plain;
    if (re->flags & SX_REG_LINE) {
        for (int k = 0; k < L->n; k++)
            if (L->len[k] == n && lit_eq(s, L->s[k], n, L->icase)) return 1;
        return 0;
    }
    if (!(re->flags & SX_REG_WORD)) {
        /* anchored strings: ^lit, lit$, ^lit$, mixed with plain ones */
        for (int k = 0; k < L->n; k++) {
            size_t len = L->len[k];
            if (len > n) continue;
            if (re->pbol[k] && re->peol[k]) {
                if (len == n && lit_eq(s, L->s[k], n, L->icase)) return 1;
            } else if (re->pbol[k]) {
                if (lit_eq(s, L->s[k], len, L->icase)) return 1;
            } else if (re->peol[k]) {
                if (lit_eq(s + n - len, L->s[k], len, L->icase)) return 1;
            } else {
                for (size_t i = 0; i + len <= n; i++)
                    if (lit_eq(s + i, L->s[k], len, L->icase)) return 1;
            }
        }
        return 0;
    }
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *c = mb_lit_find(L, p, end);
        if (!c) return 0;
        size_t at = (size_t)(c - s);
        if (at == 0 || !mb_is_word((unsigned char)s[at - 1])) {
            for (int k = 0; k < L->n; k++) {
                size_t len = L->len[k];
                if (at + len <= n && lit_eq(c, L->s[k], len, L->icase) &&
                    (at + len == n || !mb_is_word((unsigned char)s[at + len])))
                    return 1;
            }
        }
        p = c + 1;
    }
    return 0;
}

int mb_regex_line(mb_regex *re, const char *s, size_t n)
{
    if (!re || re->nothing) return 0;
    if (re->pf_on && !mb_lit_find(&re->lits, s, s + n)) return 0;
    if (re->lits.exact && re->pf_on) return 1;
    if (re->plain_ok) return plain_line(re, s, n);
    if (re->have_dfa && mb_dfa_line(sel_dfa(re), (const unsigned char *)s, n, CTX_BOL))
        return 1;
    return re->nposix ? posix_line_any(re, s, n) : 0;
}

static size_t line_start(const char *buf, size_t lo, size_t at)
{
    return mb_line_start(buf, lo, at);
}

static size_t line_end(const char *buf, size_t n, size_t at)
{
    const char *e = memchr(buf + at, '\n', n - at);
    return e ? (size_t)(e - buf) : n;
}

int mb_regex_scan(mb_regex *re, const char *buf, size_t n,
                  size_t *ls, size_t *le)
{
    size_t pos = 0;
    if (!re || re->nothing) return 0;

    if (re->pf_scan) {
        while (pos < n) {
            const char *c = mb_lit_find(&re->lits, buf + pos, buf + n);
            if (!c) return 0;
            size_t at = (size_t)(c - buf);
            size_t a = line_start(buf, pos, at);
            size_t b = line_end(buf, n, at);
            if (re->lits.exact) { *ls = a; *le = b; return 1; }
            if (re->plain_ok) {
                if (plain_line(re, buf + a, b - a)) { *ls = a; *le = b; return 1; }
                pos = b + 1;
                continue;
            }
            if ((re->have_dfa &&
                 mb_dfa_line(sel_dfa(re), (const unsigned char *)buf + a, b - a, CTX_BOL)) ||
                (re->nposix && posix_line_any(re, buf + a, b - a))) {
                *ls = a; *le = b;
                return 1;
            }
            re->pf_verified += b - a;
            re->pf_scanned  += b + 1 - pos;
            pos = b + 1;
            /* Candidates so dense that most text is verified anyway: the
             * prefilter only adds work. Scan with the DFA alone. */
            if (re->nposix == 0 && re->pf_scanned > (1u << 16) &&
                re->pf_verified * 3 > re->pf_scanned) {
                re->pf_scan = 0;
                break;
            }
        }
        if (pos >= n) return 0;
    }

    if (re->nposix == 0) {
        size_t h;
        if (!re->have_dfa ||
            !mb_dfa_scan(sel_dfa(re), (const unsigned char *)buf + pos, n - pos, &h))
            return 0;
        h += pos;
        size_t b = (h < n && buf[h] == '\n') ? h : line_end(buf, n, h);
        size_t a = line_start(buf, pos, h);
        *ls = a; *le = b;
        return 1;
    }

    while (pos < n) {
        size_t b = line_end(buf, n, pos);
        if ((re->have_dfa &&
             mb_dfa_line(sel_dfa(re), (const unsigned char *)buf + pos, b - pos, CTX_BOL)) ||
            posix_line_any(re, buf + pos, b - pos)) {
            *ls = pos; *le = b;
            return 1;
        }
        pos = b + 1;
    }
    return 0;
}

static int ends_reserve(mb_regex *re, size_t n)
{
    if (n + 2 <= re->ends_cap) return 0;
    size_t nc = re->ends_cap ? re->ends_cap : 64;
    while (nc < n + 2) nc *= 2;
    size_t *e = realloc(re->ends, nc * sizeof(size_t));
    if (!e) return -1;
    re->ends = e;
    re->ends_cap = nc;
    return 0;
}

static int bounds_dfas(mb_regex *re)
{
    if (progs(re) < 0) return -1;
    if (!re->in_un) re->in_un = mb_dfa_new(&re->in_prog, 0);
    if (!re->in_an) re->in_an = mb_dfa_new(&re->in_prog, 1);
    return re->in_un && re->in_an ? 0 : -1;
}

/* Leftmost-longest DFA match at or after from (no -w). */
static int dfa_next_plain(mb_regex *re, const unsigned char *s, size_t n,
                          size_t from, size_t *ms, size_t *me)
{
    long e = mb_dfa_first_end(re->in_un, s, n, from);
    if (e < 0) return 0;
    for (size_t st = from; st <= (size_t)e; st++) {
        long l = mb_dfa_anchored(re->in_an, s, n, st, NULL, NULL);
        if (l >= 0) { *ms = st; *me = (size_t)l; return 1; }
    }
    return 0;   /* not reached: the match ending at e starts by e */
}

/*
 * -w, as GNU grep's EGexecute() does it for -o/--color: starts in order;
 * at each start the longest match first, then shorter ones (a shorter one
 * must not be empty), until both ends are word boundaries.
 */
static int dfa_next_word(mb_regex *re, const unsigned char *s, size_t n,
                         size_t from, size_t *ms, size_t *me)
{
    if (ends_reserve(re, n) < 0) return 0;
    size_t st = from;
    while (st <= n) {
        /* sel_prog accepts a match with word boundaries on both sides:
         * no acceptable match ends before e, so none starts after it */
        long e = re->quirk ? (long)n : mb_dfa_first_end(sel_dfa(re), s, n, st);
        if (e < 0) return 0;
        for (; st <= (size_t)e; st++) {
            if (st > 0 && mb_is_word(s[st - 1])) continue;
            size_t cnt = 0;
            mb_dfa_anchored(re->in_an, s, n, st, re->ends, &cnt);
            if (cnt == 0) continue;
            /* The longest match first. Then GNU retries with re_match() on
             * the line cut short -- and computes the cut from the search
             * offset rather than the line start, so after an earlier -o
             * match (from > 0) it cuts `from` bytes too much. The next end
             * tried is the longest one at most cur-1-from, and must leave
             * a non-empty match. */
            size_t j = cnt - 1, cur = re->ends[j];
            for (;;) {
                size_t en = re->ends[j];
                if (en >= n || !mb_is_word(s[en])) {
                    *ms = st; *me = en;
                    return 1;
                }
                if (cur < 1 + from) break;
                size_t lim = cur - 1 - from;
                while (j > 0 && re->ends[j] > lim) j--;
                if (re->ends[j] > lim || re->ends[j] <= st) break;
                cur = re->ends[j];
            }
        }
    }
    return 0;
}

int mb_regex_next(mb_regex *re, const char *s, size_t n, size_t from,
                  size_t *ms, size_t *me)
{
    if (!re || re->nothing || from > n) return 0;
    const unsigned char *u = (const unsigned char *)s;
    int found = 0;
    size_t bs = 0, be = 0;

    if (re->have_dfa && bounds_dfas(re) < 0) return 0;
    if (re->have_dfa) {
        if (re->flags & SX_REG_LINE) {
            if (from == 0 &&
                mb_dfa_anchored(re->in_an, u, n, 0, NULL, NULL) == (long)n) {
                found = 1; bs = 0; be = n;
            }
        } else if (re->flags & SX_REG_WORD) {
            found = dfa_next_word(re, u, n, from, &bs, &be);
        } else {
            found = dfa_next_plain(re, u, n, from, &bs, &be);
        }
    }
    for (int k = 0; k < re->nposix; k++) {
        size_t a, b;
        if (posix_next(re, k, s, n, from, &a, &b) &&
            (!found || a < bs || (a == bs && b > be))) {
            found = 1; bs = a; be = b;
        }
    }
    if (found) { *ms = bs; *me = be; }
    return found;
}

/* ---- single-pattern API ------------------------------------------------------- */

int mb_regex_search(const mb_regex *re, const char *s, size_t n, mb_match *m)
{
    return mb_regex_search_from(re, s, n, 0, m);
}

int mb_regex_search_from(const mb_regex *cre, const char *s, size_t n,
                         size_t from, mb_match *m)
{
    mb_regex *re = (mb_regex *)cre;   /* the DFA caches are filled lazily */
    if (!re || !s || from > n) return 0;
    if (!m && from == 0) return mb_regex_line(re, s, n);
    size_t a, b;
    if (!mb_regex_next(re, s, n, from, &a, &b)) return 0;
    if (m) { m->start = s + a; m->end = s + b; }
    return 1;
}

int mb_regex_match(const mb_regex *cre, const char *s, size_t n, mb_match *m)
{
    mb_regex *re = (mb_regex *)cre;
    if (!re || !s || re->nothing) return 0;
    int ok = 0;
    if (re->have_dfa && bounds_dfas(re) < 0) return 0;
    if (re->have_dfa) {
        long l = mb_dfa_anchored(re->in_an, (const unsigned char *)s, n, 0, NULL, NULL);
        ok = (l == (long)n);
    }
    for (int k = 0; !ok && k < re->nposix; k++) {
        size_t a, b;
        ok = posix_search(re, k, s, n, 0, &a, &b, 0) && a == 0 && b == n;
    }
    if (ok && m) { m->start = s; m->end = s + n; }
    return ok;
}
