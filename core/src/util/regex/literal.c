/* literal.c — literal search for the grep prefilter, newline counting
 *
 * mb_lit_find() looks for the leftmost occurrence of any of up to
 * MB_LIT_MAX literals. For each literal two of its bytes, chosen as the
 * rarest in typical text (mb_litset_prepare), are compared against 16
 * consecutive candidate positions at once; only positions where both
 * agree are verified with a full compare. With -i a letter is compared
 * with its 0x20 bit forced on, which maps exactly its two cases together.
 *
 * SSE2 is part of the x86-64 baseline. Elsewhere a portable scalar loop
 * (memchr on the rarest byte) is used.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "regex_internal.h"
#include <ctype.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#define HAVE_SSE2 1
#endif

static const unsigned char *lower_tab(void)
{
    static unsigned char t[256];
    static int init;
    if (!init) {
        for (int i = 0; i < 256; i++) t[i] = (unsigned char)tolower(i);
        init = 1;
    }
    return t;
}

static inline int verify(const mb_litset *ls, int k, const char *c)
{
    const unsigned char *a = (const unsigned char *)c;
    const unsigned char *b = (const unsigned char *)ls->s[k];
    size_t n = ls->len[k];
    if (!ls->icase) return memcmp(a, b, n) == 0;
    const unsigned char *lt = lower_tab();
    for (size_t i = 0; i < n; i++)
        if (lt[a[i]] != b[i]) return 0;
    return 1;
}

/* Scalar search from p for literal k, candidate starts <= last. */
static const char *scalar_one(const mb_litset *ls, int k, const char *p,
                              const char *last)
{
    size_t o = ls->o1[k];
    unsigned char b = ls->b1[k];
    if (!ls->f1[k]) {
        const char *q = p + o;
        while (q <= last + o) {
            const char *h = memchr(q, b, (size_t)(last + o - q) + 1);
            if (!h) return NULL;
            if (verify(ls, k, h - o)) return h - o;
            q = h + 1;
        }
        return NULL;
    }
    unsigned char ub = (unsigned char)toupper(b);
    for (const char *c = p; c <= last; c++) {
        unsigned char x = (unsigned char)c[o];
        if ((x == b || x == ub) && verify(ls, k, c)) return c;
    }
    return NULL;
}

#ifdef HAVE_SSE2
__attribute__((nonnull(1, 3)))
static inline unsigned probe(const mb_litset *ls, int k, const char *q)
{
    __m128i v1 = _mm_loadu_si128((const __m128i *)(const void *)(q + ls->o1[k]));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(const void *)(q + ls->o2[k]));
    v1 = _mm_or_si128(v1, _mm_set1_epi8((char)ls->f1[k]));
    v2 = _mm_or_si128(v2, _mm_set1_epi8((char)ls->f2[k]));
    __m128i m = _mm_and_si128(_mm_cmpeq_epi8(v1, _mm_set1_epi8((char)ls->b1[k])),
                              _mm_cmpeq_epi8(v2, _mm_set1_epi8((char)ls->b2[k])));
    return (unsigned)_mm_movemask_epi8(m);
}

HOT static const char *find_one_sse2(const mb_litset *ls, const char *p,
                                     const char *end)
{
    if (!p) return NULL;
    size_t len = ls->len[0];
    const char *last = end - len;          /* last possible start */
    const char *q = p;
    const __m128i B1 = _mm_set1_epi8((char)ls->b1[0]);
    const __m128i B2 = _mm_set1_epi8((char)ls->b2[0]);
    const __m128i F1 = _mm_set1_epi8((char)ls->f1[0]);
    const __m128i F2 = _mm_set1_epi8((char)ls->f2[0]);
    const size_t o1 = ls->o1[0], o2 = ls->o2[0];
    /* loads read q+o..q+o+15; keep them inside the buffer */
    while (q + ls->maxprobe + 32 <= end) {
        __m128i a1 = _mm_loadu_si128((const __m128i *)(const void *)(q + o1));
        __m128i a2 = _mm_loadu_si128((const __m128i *)(const void *)(q + o2));
        __m128i c1 = _mm_loadu_si128((const __m128i *)(const void *)(q + o1 + 16));
        __m128i c2 = _mm_loadu_si128((const __m128i *)(const void *)(q + o2 + 16));
        __m128i ma = _mm_and_si128(_mm_cmpeq_epi8(_mm_or_si128(a1, F1), B1),
                                   _mm_cmpeq_epi8(_mm_or_si128(a2, F2), B2));
        __m128i mc = _mm_and_si128(_mm_cmpeq_epi8(_mm_or_si128(c1, F1), B1),
                                   _mm_cmpeq_epi8(_mm_or_si128(c2, F2), B2));
        unsigned m = (unsigned)_mm_movemask_epi8(ma) |
                     ((unsigned)_mm_movemask_epi8(mc) << 16);
        while (m) {
            const char *c = q + __builtin_ctz(m);
            if (c > last) return NULL;
            if (len <= 2 || verify(ls, 0, c)) return c;
            m &= m - 1;
        }
        q += 32;
    }
    while (q + ls->maxprobe + 16 <= end) {
        unsigned m = probe(ls, 0, q);
        while (m) {
            const char *c = q + __builtin_ctz(m);
            if (c > last) return NULL;
            if (verify(ls, 0, c)) return c;
            m &= m - 1;
        }
        q += 16;
    }
    if (q > last) return NULL;
    return scalar_one(ls, 0, q, last);
}

/* Every literal is one byte: OR the comparisons, no verification. */
HOT static const char *find_bytes_sse2(const mb_litset *ls, const char *p,
                                       const char *end)
{
    if (!p) return NULL;
    __m128i B[MB_LIT_MAX], F[MB_LIT_MAX];
    int n = ls->n;
    for (int k = 0; k < n; k++) {
        B[k] = _mm_set1_epi8((char)ls->b1[k]);
        F[k] = _mm_set1_epi8((char)ls->f1[k]);
    }
    const char *q = p;
    while (q + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)q);
        __m128i m = _mm_cmpeq_epi8(_mm_or_si128(v, F[0]), B[0]);
        for (int k = 1; k < n; k++)
            m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_or_si128(v, F[k]), B[k]));
        unsigned mm = (unsigned)_mm_movemask_epi8(m);
        if (mm) return q + __builtin_ctz(mm);
        q += 16;
    }
    for (; q < end; q++) {
        unsigned char c = (unsigned char)*q;
        for (int k = 0; k < n; k++)
            if ((unsigned char)(c | ls->f1[k]) == ls->b1[k]) return q;
    }
    return NULL;
}

HOT static const char *find_multi_sse2(const mb_litset *ls, const char *p,
                                       const char *end)
{
    if (!p) return NULL;
    __m128i B1[MB_LIT_MAX], B2[MB_LIT_MAX], F1[MB_LIT_MAX], F2[MB_LIT_MAX];
    int n = ls->n;
    for (int k = 0; k < n; k++) {
        B1[k] = _mm_set1_epi8((char)ls->b1[k]);
        B2[k] = _mm_set1_epi8((char)ls->b2[k]);
        F1[k] = _mm_set1_epi8((char)ls->f1[k]);
        F2[k] = _mm_set1_epi8((char)ls->f2[k]);
    }
    const char *q = p;
    while (q + ls->maxprobe + 16 <= end) {
        unsigned any = 0, mk[MB_LIT_MAX];
        for (int k = 0; k < n; k++) {
            __m128i v1 = _mm_loadu_si128((const __m128i *)(const void *)(q + ls->o1[k]));
            __m128i v2 = _mm_loadu_si128((const __m128i *)(const void *)(q + ls->o2[k]));
            __m128i m = _mm_and_si128(_mm_cmpeq_epi8(_mm_or_si128(v1, F1[k]), B1[k]),
                                      _mm_cmpeq_epi8(_mm_or_si128(v2, F2[k]), B2[k]));
            mk[k] = (unsigned)_mm_movemask_epi8(m);
            any |= mk[k];
        }
        if (any) {
            const char *best = NULL;
            for (int k = 0; k < n; k++) {
                unsigned m = mk[k];
                while (m) {
                    const char *c = q + __builtin_ctz(m);
                    if (best && c >= best) break;
                    if (c + ls->len[k] <= end &&
                        (ls->len[k] <= 2 || verify(ls, k, c))) { best = c; break; }
                    m &= m - 1;
                }
            }
            if (best) return best;
        }
        q += 16;
    }
    /* tail: scalar */
    const char *best = NULL;
    for (int k = 0; k < n; k++) {
        if ((size_t)(end - q) < ls->len[k]) continue;
        const char *lim = best ? best - 1 : end - ls->len[k];
        if (lim > end - ls->len[k]) lim = end - ls->len[k];
        if (q > lim) continue;
        const char *c = scalar_one(ls, k, q, lim);
        if (c && (!best || c < best)) best = c;
    }
    return best;
}
#endif

const char *mb_lit_find(const mb_litset *ls, const char *p, const char *end)
{
    if (ls->n <= 0 || p >= end) return NULL;
#ifdef HAVE_SSE2
    if (ls->maxlen == 1) return find_bytes_sse2(ls, p, end);
    if (ls->n == 1) {
        if ((size_t)(end - p) < ls->len[0]) return NULL;
        return find_one_sse2(ls, p, end);
    }
    return find_multi_sse2(ls, p, end);
#else
    const char *best = NULL;
    for (int k = 0; k < ls->n; k++) {
        if ((size_t)(end - p) < ls->len[k]) continue;
        const char *lim = end - ls->len[k];
        if (best && best - 1 < lim) lim = best - 1;
        if (p > lim) continue;
        const char *c = scalar_one(ls, k, p, lim);
        if (c && (!best || c < best)) best = c;
    }
    return best;
#endif
}

/* Start of the line holding buf[at]: just after the last '\n' in
 * buf[lo..at), or lo. */
size_t mb_line_start(const char *buf, size_t lo, size_t at)
{
#ifdef HAVE_SSE2
    const __m128i NL = _mm_set1_epi8('\n');
    while (at >= lo + 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(buf + at - 16));
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, NL));
        if (m) return at - 16 + (size_t)(31 - __builtin_clz(m)) + 1;
        at -= 16;
    }
#endif
    while (at > lo && buf[at - 1] != '\n') at--;
    return at;
}

size_t mb_count_nl(const char *p, size_t n)
{
    size_t cnt = 0, i = 0;
#ifdef HAVE_SSE2
    const __m128i NL = _mm_set1_epi8('\n');
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
        cnt += (size_t)__builtin_popcount((unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, NL)));
    }
#endif
    for (; i < n; ) {
        const char *h = memchr(p + i, '\n', n - i);
        if (!h) break;
        cnt++;
        i = (size_t)(h - p) + 1;
    }
    return cnt;
}
