/* grep.c — grep builtin: search files for patterns
 *
 * Input is read in large blocks straight from the file descriptor. With no
 * context options, a whole block of lines is handed to the regex engine,
 * which finds the next matching line itself (literal prefilter + DFA), so
 * lines that cannot match are never visited one by one; with -v the lines
 * between matches are written out as one run. Context (-A/-B/-C) and files
 * that turn out to be binary are processed line by line.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/opt.h"
#include "../util/path.h"
#include "../util/strbuf.h"
#include "../util/regex/regex.h"
#include "../util/vcsignore.h"
#include "../module/registry.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Maximum number of -e patterns or -f pattern files */
#define MAX_PATTERNS 256
/* Maximum number of --include/--exclude globs each */
#define MAX_GLOBS    64
/* Input is read in blocks of this size (the buffer grows for longer
 * lines). 256 KB: ~190 read() calls for a 50 MB file. */
#define GREP_READ_CHUNK (1 << 18)
#define GREP_READ_SMALL (1 << 15)
#define GREP_KEEP_BUF   (1 << 20)

/* ------------------------------------------------------------------ */
/* Option state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int opt_E;           /* -E: extended (ERE) */
    int opt_F;           /* -F: fixed string */
    int opt_G;           /* -G: basic (BRE), the default */
    int opt_c;           /* -c: count matches */
    int opt_i;           /* -i: case insensitive */
    int opt_l;           /* -l: list filenames only */
    int opt_L;           /* -L: list files with NO match */
    int opt_H;           /* -H: always print filename prefix */
    int opt_h;           /* -h: never print filename prefix */
    int opt_n;           /* -n: print line numbers */
    int opt_q;           /* -q: quiet */
    int opt_s;           /* -s: suppress file errors */
    int opt_v;           /* -v: invert match */
    int opt_w;           /* -w: word boundaries */
    int opt_r;           /* -r/-R: recursive */
    int opt_color;       /* --color: ANSI highlight */
    int opt_o;           /* -o: only matching portion */
    int opt_x;           /* -x: match whole line */
    int opt_b;           /* -b: print byte offset */
    int opt_Z;           /* -Z: NUL-terminate filenames */
    int max_matches;     /* -m N: stop after N matches (<= 0 = unlimited) */
    int max_given;       /* -m seen (-m 0: nothing can be selected) */
    int context_A;       /* -A N: N lines of after-context */
    int context_B;       /* -B N: N lines of before-context */

    /* Patterns */
    char  *patterns[MAX_PATTERNS];
    int    npatterns;
    int    pattern_given;  /* -e or -f seen (-f /dev/null: no patterns) */

    /* All patterns, compiled as one set */
    mb_regex *re;

    /* Glob filters */
    char *include_globs[MAX_GLOBS];
    int   n_include;
    char *exclude_globs[MAX_GLOBS];
    int   n_exclude;

    /* --vcs: skip VCS dirs, hidden files, binary files */
    int opt_vcs;
    /* -S: smart case (case-insensitive when pattern is all-lowercase) */
    int opt_smart;
    /* VCS ignore context (loaded when opt_vcs) */
    vcsignore_t *vcs_ign;

    /* stdout is /dev/null: the first selected line of a file decides
     * everything observable, so stop reading there (as GNU grep does) */
    int devnull;
    /* the patterns match every line */
    int all_lines;

    /* input buffer, reused for every file of the run */
    char  *rbuf;
    size_t rcap;
} grep_opts_t;

/* ------------------------------------------------------------------ */
/* Global state shared across recursive traversal                     */
/* ------------------------------------------------------------------ */

static int g_matched_any = 0; /* set to 1 if any match ever found */
/* input buffer kept between runs (see grep_opts_free) */
static char  *g_rbuf;
static size_t g_rcap;
/* A context group was printed for an earlier file in this run: the next
 * group, even in another file, is preceded by "--" (GNU). Reset per run. */
static int g_ctx_output = 0;

/* ------------------------------------------------------------------ */
/* Patterns                                                            */
/* ------------------------------------------------------------------ */

static void grep_warn(const char *msg)
{
    err_msg("grep", "warning: %s", msg);
}

/* Compile all patterns into one mb_regex set. */
static int compile_patterns(grep_opts_t *g)
{
    /* Smart case (-S): if all patterns are all-lowercase, enable -i.
     * Explicit -i always wins. */
    if (g->opt_smart && !g->opt_i) {
        int all_lower = 1;
        for (int j = 0; j < g->npatterns && all_lower; j++) {
            for (const char *c = g->patterns[j]; *c; c++) {
                if (isupper((unsigned char)*c)) { all_lower = 0; break; }
            }
        }
        if (all_lower) g->opt_i = 1;
    }

    int flags = 0;
    if (g->opt_F)      flags |= SX_REG_FIXED;
    else if (g->opt_E) flags |= SX_REG_ERE;
    if (g->opt_i)      flags |= SX_REG_ICASE;
    if (g->opt_x)      flags |= SX_REG_LINE;     /* -x wins over -w (GNU) */
    else if (g->opt_w) flags |= SX_REG_WORD;

    const char *err = NULL;
    int ep = 0;
    g->re = mb_regex_compile_set((const char *const *)g->patterns, g->npatterns,
                                 flags, &err, &ep, grep_warn);
    if (!g->re) {
        err_msg("grep", "invalid regex '%s': %s",
                (ep >= 0 && ep < g->npatterns) ? g->patterns[ep] : "",
                err ? err : "unknown error");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/* ANSI color codes */
#define COL_MATCH   "\033[01;31m"
#define COL_FNAME   "\033[35m"
#define COL_LINENO  "\033[32m"
#define COL_SEP     "\033[36m"
#define COL_RESET   "\033[0m"

static inline void out(const char *p, size_t n)
{
    if (n) fwrite(p, 1, n, stdout);
}

/* Write a line and its newline; the newline is in the buffer when the line
 * has one, so it goes out in the same call. */
static inline void out_line(const char *p, size_t n, int has_nl)
{
    if (has_nl) {
        fwrite(p, 1, n + 1, stdout);
    } else {
        out(p, n);
        putc('\n', stdout);
    }
}

static char *fmt_ulong(char *end, unsigned long long v)
{
    do { *--end = (char)('0' + v % 10); v /= 10; } while (v);
    return end;
}

/* Build the line prefix (filename, line number, byte offset) in out.
 * GNU order: FILE:LINE:OFFSET: */
static int grep_print_prefix(strbuf_t *sb, const char *filename,
                             int show_fname, long linenum, long long byte_offset,
                             const grep_opts_t *g, int is_context)
{
    char sep = is_context ? '-' : ':';
    char num[32];
    if (show_fname && filename) {
        if (g->opt_color) {
            if (sb_append(sb, COL_FNAME) != 0) return -1;
            if (sb_append(sb, filename)  != 0) return -1;
            if (sb_append(sb, COL_RESET) != 0) return -1;
        } else {
            if (sb_append(sb, filename) != 0) return -1;
        }
        if (sb_appendc(sb, sep) != 0) return -1;
    }
    if (g->opt_n) {
        char *s = fmt_ulong(num + sizeof(num), (unsigned long long)linenum);
        if (g->opt_color && sb_append(sb, COL_LINENO) != 0) return -1;
        if (sb_appendn(sb, s, (size_t)(num + sizeof(num) - s)) != 0) return -1;
        if (g->opt_color && sb_append(sb, COL_RESET) != 0) return -1;
        if (sb_appendc(sb, sep) != 0) return -1;
    }
    if (g->opt_b) {
        char *s = fmt_ulong(num + sizeof(num), (unsigned long long)byte_offset);
        if (sb_appendn(sb, s, (size_t)(num + sizeof(num) - s)) != 0) return -1;
        if (sb_appendc(sb, sep) != 0) return -1;
    }
    return 0;
}

/*
 * Print line with every match highlighted (--color).
 * The tail after the last match is written once.
 */
static void print_line_color(const char *line, size_t len, const grep_opts_t *g)
{
    size_t done = 0, from = 0, ms, me;

    while (from <= len && mb_regex_next(g->re, line, len, from, &ms, &me)) {
        if (me == ms) { from = ms + 1; continue; }   /* nothing to show */
        out(line + done, ms - done);
        fputs(COL_MATCH, stdout);
        out(line + ms, me - ms);
        fputs(COL_RESET, stdout);
        done = from = me;
    }
    out(line + done, len - done);
}

/* Print matched portions of line (for -o flag). With -b each match is
 * prefixed with its own byte offset, as in GNU grep. */
static int grep_print_only_match(const char *line, size_t len,
                                 const grep_opts_t *g, strbuf_t *sb,
                                 const char *filename, int show_fname,
                                 long linenum, long long line_off)
{
    size_t from = 0, ms, me;

    sb_reset(sb);
    if (grep_print_prefix(sb, filename, show_fname, linenum, line_off, g, 0) < 0)
        return -1;
    while (from <= len && mb_regex_next(g->re, line, len, from, &ms, &me)) {
        if (me == ms) { from = ms + 1; continue; }  /* GNU skips empty matches */
        if (g->opt_b) {
            sb_reset(sb);
            if (grep_print_prefix(sb, filename, show_fname, linenum,
                                  line_off + (long long)ms, g, 0) < 0)
                return -1;
        }
        out(sb_str(sb), sb_len(sb));
        if (g->opt_color) fputs(COL_MATCH, stdout);
        out(line + ms, me - ms);
        if (g->opt_color) fputs(COL_RESET, stdout);
        putc('\n', stdout);
        from = me;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int     fd;
    char   *buf;
    size_t  cap, len, pos;   /* data buf[0..len), processed up to pos */
    long long base;          /* stream offset of buf[0] */
    int     eof;
    int     nreads;
    size_t  nul;             /* offset of a NUL byte (SIZE_MAX: none seen) */
} rd_t;

/* Read more input after the unprocessed tail. 1: got data, 0: EOF,
 * -1: read error (errno set). */
static int rd_fill(rd_t *r)
{
    if (r->pos > 0) {
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->base += (long long)r->pos;
        if (r->nul != SIZE_MAX) r->nul = r->nul >= r->pos ? r->nul - r->pos : 0;
        r->pos = 0;
    }
    /* Start small (most files grep sees are), go to full-size reads from
     * the third one on, and grow further only for lines that need it. */
    size_t want = r->nreads >= 2 ? GREP_READ_CHUNK / 2 : GREP_READ_SMALL / 2;
    if (r->cap - r->len < want || (r->nreads >= 2 && r->cap < GREP_READ_CHUNK)) {
        size_t nc = r->cap ? r->cap * 2 : GREP_READ_SMALL;
        if (r->nreads >= 2 && nc < GREP_READ_CHUNK) nc = GREP_READ_CHUNK;
        while (nc - r->len < want) nc *= 2;
        char *nb = realloc(r->buf, nc);
        if (!nb) { errno = ENOMEM; return -1; }
        r->buf = nb;
        r->cap = nc;
    }
    ssize_t n;
    do {
        n = read(r->fd, r->buf + r->len, r->cap - r->len);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (n == 0) { r->eof = 1; return 0; }
    r->nreads++;
    if (r->nul == SIZE_MAX) {
        const char *z = memchr(r->buf + r->len, '\0', (size_t)n);
        if (z) r->nul = (size_t)(z - r->buf);
    }
    r->len += (size_t)n;
    return 1;
}

/* Make buf[pos..*lim) a non-empty run of whole lines (the last line of
 * the input may lack its newline). 1: ok, 0: end of input, -1: error. */
static int rd_lines(rd_t *r, size_t *lim)
{
    size_t lo = r->pos;    /* [pos, lo) is known to hold no newline */
    for (;;) {
        size_t i = r->len;
        while (i > lo && r->buf[i - 1] != '\n') i--;
        if (i > lo) { *lim = i; return 1; }
        lo = r->len;
        if (r->eof) {
            if (r->len > r->pos) { *lim = r->len; return 1; }
            return 0;
        }
        size_t shift = r->pos;
        if (rd_fill(r) < 0) return -1;
        lo -= shift;
    }
}

/* ------------------------------------------------------------------ */
/* Search                                                              */
/* ------------------------------------------------------------------ */

enum { SEL_GO, SEL_STOP, SEL_DONE, SEL_OOM };

typedef struct {
    const char *filename;
    int   show_fname;
    long  count;
    long  linenum;
    int   result;
    int   stop_early;      /* the first selected line decides the result */
    strbuf_t out;
} sel_t;

/*
 * A selected line in the fast path (no context): count it, and print it
 * unless -c/-l/-L/-q. SEL_STOP: stop reading (-m, or the result is
 * decided); SEL_DONE: return at once (-q, -l).
 */
static int select_line(grep_opts_t *g, sel_t *s, const char *line, size_t len,
                       int has_nl, long long off)
{
    s->count++;
    s->result = 0;
    g_matched_any = 1;

    if (g->opt_q) return SEL_DONE;
    if (g->opt_L) return s->stop_early ? SEL_STOP : SEL_GO;
    if (g->opt_l) {
        if (g->opt_Z && s->filename) {
            fputs(s->filename, stdout);
            putc('\0', stdout);
        } else {
            fputs(s->filename ? s->filename : "(stdin)", stdout);
            putc('\n', stdout);
        }
        return SEL_DONE;
    }
    if (s->stop_early) return SEL_STOP;
    if (g->opt_c)
        return (g->max_matches > 0 && s->count >= g->max_matches) ? SEL_STOP : SEL_GO;

    if (g->opt_o) {
        if (!g->opt_v &&
            grep_print_only_match(line, len, g, &s->out, s->filename,
                                  s->show_fname, s->linenum, off) < 0)
            return SEL_OOM;
    } else {
        if (s->show_fname || g->opt_n || g->opt_b) {
            sb_reset(&s->out);
            if (grep_print_prefix(&s->out, s->filename, s->show_fname,
                                  s->linenum, off, g, 0) < 0) return SEL_OOM;
            out(sb_str(&s->out), sb_len(&s->out));
        }
        if (g->opt_color && !g->opt_v) {
            print_line_color(line, len, g);
            putc('\n', stdout);
        } else {
            out_line(line, len, has_nl);
        }
    }
    if (g->max_matches > 0 && s->count >= g->max_matches) return SEL_STOP;
    return SEL_GO;
}

/* Save a line in the before-context ring */
static int ring_put(char **buf, size_t *blen, long *lnum, long long *boff,
                    int *pos, int *full, int n, const char *line, size_t len,
                    long linenum, long long off)
{
    char *c = malloc(len + 1);
    if (!c) return -1;
    memcpy(c, line, len);
    c[len] = '\0';
    free(buf[*pos]);
    buf[*pos]  = c;
    blen[*pos] = len;
    lnum[*pos] = linenum;
    boff[*pos] = off;
    *pos = (*pos + 1) % n;
    if (!*full && *pos == 0) *full = 1;
    return 0;
}

/*
 * Search one open file descriptor for matches.
 * filename: displayed name (NULL if reading from stdin with no label).
 * show_fname: prepend filename to output lines.
 * Returns 0 if matches found, 1 if none, 2 on read error.
 */
static int grep_stream(int fd, const char *filename, int show_fname,
                       grep_opts_t *g, int is_stdin)
{
    rd_t r;
    memset(&r, 0, sizeof(r));
    r.fd  = fd;
    r.buf = g->rbuf;
    r.cap = g->rcap;
    r.nul = SIZE_MAX;

    sel_t s;
    memset(&s, 0, sizeof(s));
    s.filename   = filename;
    s.show_fname = show_fname;
    s.result     = 1;   /* assume no match */
    /* Once a line is selected nothing more can be observed: output to
     * /dev/null, or -L (which prints only files WITHOUT a match). Not for
     * stdin, whose read offset the next command sees. */
    s.stop_early = !is_stdin && (g->devnull || (g->opt_L && !g->opt_c));

    int binary = 0;
    int binary_matched = 0;   /* pattern found within a binary file */
    int stopped = 0;
    int ret;

    /* Context support */
    int ctx_B = g->context_B;
    int ctx_A = g->context_A;
    int after_left  = 0;
    long last_printed = -1;
    int line_mode = ctx_A > 0 || ctx_B > 0;

    char      **before_buf  = NULL;
    size_t     *before_len  = NULL;
    long       *before_lnum = NULL;
    long long  *before_boff = NULL;  /* byte offsets for -b */
    int         before_pos  = 0;
    int         before_full = 0;
    if (ctx_B > 0) {
        before_buf  = calloc((size_t)ctx_B, sizeof(char *));
        before_len  = calloc((size_t)ctx_B, sizeof(size_t));
        before_lnum = calloc((size_t)ctx_B, sizeof(long));
        before_boff = calloc((size_t)ctx_B, sizeof(long long));
        if (!before_buf || !before_len || !before_lnum || !before_boff) {
            free(before_buf); free(before_len); free(before_lnum); free(before_boff);
            err_msg("grep", "out of memory");
            return 2;
        }
    }

    if (sb_init(&s.out, 256) != 0) {
        free(before_buf); free(before_len); free(before_lnum); free(before_boff);
        err_msg("grep", "out of memory");
        return 2;
    }

    for (;;) {
        size_t lim;
        int st = rd_lines(&r, &lim);
        if (st < 0) goto read_error;
        if (st == 0) break;

        if (!line_mode) {
            /* Fast path. Stop before a line holding a NUL byte: from there
             * the file is binary and goes line by line. */
            size_t stop = lim;
            if (r.nul != SIZE_MAX && r.nul < lim) {
                stop = r.nul;
                while (stop > r.pos && r.buf[stop - 1] != '\n') stop--;
                line_mode = 1;
            }
            const char *B = r.buf;
            size_t p = r.pos, cnt_pos = r.pos;
            if (g->all_lines && p < stop) {
                /* Every line matches (x*, the empty pattern...) */
                int unterm = B[stop - 1] != '\n';
                if (g->opt_v) {
                    p = stop;                       /* nothing selected */
                } else if (g->opt_c && !g->max_matches && !s.stop_early) {
                    s.count += (long)mb_regex_count_nl(B + p, stop - p) + unterm;
                    s.result = 0;
                    g_matched_any = 1;
                    p = stop;
                } else if (!g->opt_n && !g->opt_b && !show_fname && !g->opt_o &&
                           !g->opt_color && !g->opt_c && !g->opt_q && !g->opt_l &&
                           !g->opt_L && !g->max_matches && !s.stop_early) {
                    s.count++;
                    s.result = 0;
                    g_matched_any = 1;
                    out(B + p, stop - p);
                    if (unterm) putc('\n', stdout);
                    p = stop;
                }
            }
            while (p < stop) {
                size_t ls = 0, le = 0;
                int f = mb_regex_scan(g->re, B + p, stop - p, &ls, &le);
                ls += p;
                le += p;
                size_t gap_end = f ? ls : stop;

                if (g->opt_v && gap_end > p) {
                    /* every line in [p, gap_end) is selected */
                    if (!g->opt_n && !g->opt_b && !show_fname && !g->opt_o &&
                        !g->opt_c && !g->opt_q && !g->opt_l && !g->opt_L &&
                        !g->max_matches && !s.stop_early) {
                        int nl_end = B[gap_end - 1] == '\n';
                        s.count++;
                        s.result = 0;
                        g_matched_any = 1;
                        out(B + p, gap_end - p);
                        if (!nl_end) putc('\n', stdout);
                    } else if (g->opt_c && !g->max_matches && !s.stop_early) {
                        s.count += (long)mb_regex_count_nl(B + p, gap_end - p);
                        if (B[gap_end - 1] != '\n') s.count++;
                        s.result = 0;
                        g_matched_any = 1;
                    } else {
                        while (p < gap_end) {
                            const char *e = memchr(B + p, '\n', gap_end - p);
                            size_t le2 = e ? (size_t)(e - B) : gap_end;
                            if (g->opt_n) {
                                s.linenum += (long)mb_regex_count_nl(B + cnt_pos, p - cnt_pos) + 1;
                                cnt_pos = e ? le2 + 1 : gap_end;
                            }
                            int act = select_line(g, &s, B + p, le2 - p, e != NULL,
                                                  r.base + (long long)p);
                            p = e ? le2 + 1 : gap_end;
                            if (act == SEL_OOM) goto oom;
                            if (act == SEL_DONE) { r.pos = p; goto done; }
                            if (act == SEL_STOP) { stopped = 1; break; }
                        }
                        if (stopped) break;
                    }
                }
                if (!f) { p = stop; break; }

                size_t a = ls, b = le;
                int has_nl = b < stop;
                size_t nxt = has_nl ? b + 1 : stop;
                if (!g->opt_v) {
                    if (g->opt_n) {
                        s.linenum += (long)mb_regex_count_nl(B + cnt_pos, a - cnt_pos) + 1;
                        cnt_pos = nxt;
                    }
                    int act = select_line(g, &s, B + a, b - a, has_nl,
                                          r.base + (long long)a);
                    p = nxt;
                    if (act == SEL_OOM) goto oom;
                    if (act == SEL_DONE) { r.pos = p; goto done; }
                    if (act == SEL_STOP) { stopped = 1; break; }
                } else {
                    p = nxt;
                }
            }
            if (g->opt_n && !stopped)
                s.linenum += (long)mb_regex_count_nl(B + cnt_pos, p - cnt_pos);
            r.pos = p;
            if (stopped) break;
            if (!line_mode) continue;
            lim = (r.pos < lim) ? lim : r.pos;
        }

        /* Line by line: context, binary files */
        while (r.pos < lim) {
            char  *line = r.buf + r.pos;
            char  *e    = memchr(line, '\n', lim - r.pos);
            size_t linelen = e ? (size_t)(e - line) : lim - r.pos;
            long long line_start_offset = r.base + (long long)r.pos;
            r.pos += linelen + (e ? 1 : 0);
            s.linenum++;
            long linenum = s.linenum;

            /* Binary detection: check for NUL bytes. A binary file is still
             * SEARCHED -- GNU grep only reports "Binary file matches" when the
             * pattern is actually present (exit 1 with no output otherwise).
             * Match each NUL-separated segment. */
            if (!binary && memchr(line, '\0', linelen)) binary = 1;
            if (binary) {
                if (!binary_matched) {
                    const char *seg = line, *bend = line + linelen;
                    for (;;) {
                        const char *z = memchr(seg, '\0', (size_t)(bend - seg));
                        size_t sl = z ? (size_t)(z - seg) : (size_t)(bend - seg);
                        if (sl > 0 && mb_regex_line(g->re, seg, sl)) {
                            binary_matched = 1;
                            break;
                        }
                        if (!z) break;
                        seg = z + 1;
                    }
                }
                if (s.stop_early && binary_matched && !g->opt_v) { stopped = 1; break; }
                continue;
            }

            int matched = mb_regex_line(g->re, line, linelen);
            if (g->opt_v) matched = !matched;

            /* Past the -m limit (only reachable with -A): GNU prints further
             * lines, matching or not, as trailing context, then stops. */
            int past_limit = g->max_matches > 0 && s.count >= g->max_matches;
            if (past_limit) matched = 0;

            if (!matched) {
                /* Print as after-context if needed */
                if (after_left > 0 && !g->opt_c && !g->opt_q && !g->opt_l && !g->opt_L) {
                    /* With -o, GNU prints no context lines, but they still
                     * join groups, so "--" appears only between real gaps. */
                    if (!g->opt_o) {
                        sb_reset(&s.out);
                        if (grep_print_prefix(&s.out, filename, show_fname,
                                              linenum, line_start_offset, g, 1) < 0) goto oom;
                        out(sb_str(&s.out), sb_len(&s.out));
                        out(line, linelen);
                        putc('\n', stdout);
                    }
                    last_printed = linenum;
                    after_left--;
                }
                /* -m: the trailing context is done, stop reading (so the rest
                 * of a seekable stdin is left for the next command). */
                if (past_limit && after_left <= 0) { stopped = 1; break; }
                /* Store in before-context ring buffer */
                if (ctx_B > 0 &&
                    ring_put(before_buf, before_len, before_lnum, before_boff,
                             &before_pos, &before_full, ctx_B, line, linelen,
                             linenum, line_start_offset) < 0) goto oom;
                continue;
            }

            s.count++;
            s.result = 0;
            g_matched_any = 1;

            if (g->opt_q) goto done;
            if (g->opt_L) {
                if (s.stop_early) { stopped = 1; break; }
                continue; /* suppress output; track match status only */
            }
            if (g->opt_l) {
                if (g->opt_Z && filename) {
                    fputs(filename, stdout);
                    putc('\0', stdout);
                } else {
                    fputs(filename ? filename : "(stdin)", stdout);
                    putc('\n', stdout);
                }
                goto done;
            }
            if (s.stop_early) { stopped = 1; break; }
            if (g->opt_c) {
                if (ctx_B > 0 &&
                    ring_put(before_buf, before_len, before_lnum, before_boff,
                             &before_pos, &before_full, ctx_B, line, linelen,
                             linenum, line_start_offset) < 0) goto oom;
                if (g->max_matches > 0 && s.count >= g->max_matches) { stopped = 1; break; }
                continue;
            }

            /* Print separator between context groups. The group starts at
             * the first before-context line still to be printed. */
            int n_avail = 0, bstart = 0;
            long first_line = linenum;
            if (ctx_B > 0) {
                n_avail = before_full ? ctx_B : before_pos;
                bstart  = before_full ? before_pos : 0;
                for (int k = 0; k < n_avail; k++) {
                    int idx = (bstart + k) % ctx_B;
                    if (before_buf[idx] && before_lnum[idx] > last_printed) {
                        first_line = before_lnum[idx];
                        break;
                    }
                }
            }
            if ((ctx_A > 0 || ctx_B > 0) &&
                ((last_printed >= 0 && last_printed < first_line - 1) ||
                 (last_printed < 0 && g_ctx_output))) {
                fputs("--\n", stdout);
            }
            g_ctx_output = 1;

            /* Print before-context lines (not with -o, as in GNU) */
            if (ctx_B > 0) {
                for (int k = 0; k < n_avail; k++) {
                    int idx = (bstart + k) % ctx_B;
                    if (!before_buf[idx] || before_lnum[idx] <= last_printed) continue;
                    if (!g->opt_o) {
                        sb_reset(&s.out);
                        if (grep_print_prefix(&s.out, filename, show_fname,
                                              before_lnum[idx], before_boff[idx], g, 1) < 0) goto oom;
                        out(sb_str(&s.out), sb_len(&s.out));
                        out(before_buf[idx], before_len[idx]);
                        putc('\n', stdout);
                    }
                    last_printed = before_lnum[idx];
                }
                for (int k = 0; k < ctx_B; k++) {
                    free(before_buf[k]); before_buf[k] = NULL;
                    before_lnum[k] = 0; before_boff[k] = 0; before_len[k] = 0;
                }
                before_pos = 0; before_full = 0;
            }

            /* Print this matched line */
            if (g->opt_o) {
                if (!g->opt_v &&
                    grep_print_only_match(line, linelen, g, &s.out, filename,
                                          show_fname, linenum,
                                          line_start_offset) < 0)
                    goto oom;
            } else {
                sb_reset(&s.out);
                if (grep_print_prefix(&s.out, filename, show_fname, linenum,
                                      line_start_offset, g, 0) < 0) goto oom;
                out(sb_str(&s.out), sb_len(&s.out));
                if (g->opt_color && !g->opt_v && g->npatterns > 0)
                    print_line_color(line, linelen, g);
                else
                    out(line, linelen);
                putc('\n', stdout);
            }
            last_printed = linenum;
            after_left   = ctx_A;

            if (ctx_B > 0 &&
                ring_put(before_buf, before_len, before_lnum, before_boff,
                         &before_pos, &before_full, ctx_B, line, linelen,
                         linenum, line_start_offset) < 0) goto oom;

            /* -m N: stop reading right after the Nth match (GNU), so the rest of
             * a seekable stdin is left for the next reader -- `{ grep -m1 x;
             * cat; } <f`. With -A, keep going for the trailing context. */
            if (g->max_matches > 0 && s.count >= g->max_matches && ctx_A == 0) {
                stopped = 1;
                break;
            }
        }
        if (stopped) break;
    }

    if (binary) {
        int bin_hit = binary_matched;
        if (g->opt_v) bin_hit = !bin_hit;
        if (bin_hit) {
            if (!g->opt_q && !g->opt_l && !g->opt_c) {
                printf("Binary file %s matches\n",
                       filename ? filename : "(stdin)");
            } else if (g->opt_l && filename) {
                printf("%s\n", filename);
            }
            s.result = 0;
            g_matched_any = 1;
        }
        /* else: a text line before the first NUL may already have matched
         * (and been printed); that still counts as a match. */
    }

    if (g->opt_c) {
        if (show_fname && filename)
            printf("%s:%ld\n", filename, s.count);
        else
            printf("%ld\n", s.count);
        if (s.count > 0)
            s.result = 0;
    }

done:
    ret = s.result;
    goto cleanup;

read_error:
    if (!g->opt_s && filename)
        err_sys("grep", "%s", filename);
    ret = 2;
    goto cleanup;

oom:
    err_msg("grep", "out of memory building output line");
    ret = 2;

cleanup:
    /* Stopped early: hand the unread input back to a seekable stdin, so
     * the next command reads on from just after the last line used. */
    if (is_stdin && r.pos < r.len)
        (void)lseek(fd, -(off_t)(r.len - r.pos), SEEK_CUR);
    if (ctx_B > 0) {
        for (int k = 0; k < ctx_B; k++) free(before_buf[k]);
        free(before_buf); free(before_len); free(before_lnum); free(before_boff);
    }
    sb_free(&s.out);
    g->rbuf = r.buf;
    g->rcap = r.cap;
    return ret;
}

/* Check if filename matches any exclude glob or fails include globs. */
static int glob_excluded(const char *basename, const grep_opts_t *g)
{
    for (int i = 0; i < g->n_exclude; i++) {
        if (fnmatch(g->exclude_globs[i], basename, 0) == 0)
            return 1;
    }
    if (g->n_include > 0) {
        int ok = 0;
        for (int i = 0; i < g->n_include; i++) {
            if (fnmatch(g->include_globs[i], basename, 0) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return 1;
    }
    return 0;
}

/* Forward declaration for recursive use */
static int grep_path(const char *path, int show_fname, int in_recursion,
                    grep_opts_t *g);

/* Recursively grep a directory. */
static int grep_dir(const char *dirpath, grep_opts_t *g)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (!g->opt_s)
            err_sys("grep", "%s", dirpath);
        return 2;
    }

    int result = 1;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* --vcs: skip hidden entries and VCS/noise dirs */
        if (g->opt_vcs) {
            if (ent->d_name[0] == '.') continue; /* hidden */
            int is_dir = 0;
#ifdef DT_DIR
            if (ent->d_type == DT_DIR)      is_dir = 1;
            else if (ent->d_type == DT_UNKNOWN) {
                struct stat st2;
                char tmp[PATH_MAX];
                if (path_join(dirpath, ent->d_name, tmp) && lstat(tmp, &st2) == 0)
                    is_dir = S_ISDIR(st2.st_mode);
            }
#endif
            if (vcsignore_skip_name(ent->d_name, is_dir)) continue;
            /* Check .gitignore rules against basename (handles *.ext, dir/) */
            if (g->vcs_ign && vcsignore_match(g->vcs_ign, ent->d_name, is_dir))
                continue;
        }

        char child[PATH_MAX];
        if (!path_join(dirpath, ent->d_name, child)) {
            err_msg("grep", "path too long: %s/%s", dirpath, ent->d_name);
            continue;
        }

        int r = grep_path(child, 1, 1, g);
        if (r == 0)
            result = 0;
    }

    closedir(dir);
    return result;
}

/* Grep one path (file or directory). show_fname: prepend filename. */
static int grep_path(const char *path, int show_fname, int in_recursion,
                    grep_opts_t *g)
{
    /* POSIX: "-" means stdin */
    if (strcmp(path, "-") == 0) {
        return grep_stream(STDIN_FILENO, "(standard input)", show_fname, g, 1);
    }

    /* stat() for a path the user NAMED, lstat() only while recursing.
     *
     * This was lstat() unconditionally, and lstat() reports the LINK, so
     * S_ISREG was false for every symlink and the !S_ISREG arm below returned
     * "no match" -- silently, with no diagnostic and exit status 1. On Debian
     * /etc/os-release is a symlink to ../usr/lib/os-release, so
     * `grep -q ID_LIKE=debian /etc/os-release` answered "no" on a file that
     * plainly says yes. Any configure script probing a symlinked path got the
     * same wrong answer, and nothing anywhere said so.
     *
     * GNU's rule, which this now follows: a symlink named on the command line
     * is followed; symlinks FOUND while walking a tree under -r are not, so a
     * link back up the tree cannot make the walk loop forever. */
    struct stat st;
    int strc = in_recursion ? lstat(path, &st) : stat(path, &st);
    if (strc != 0) {
        if (!g->opt_s)
            err_sys("grep", "%s", path);
        return 2;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!g->opt_r) {
            if (!g->opt_s)
                err_msg("grep", "%s: is a directory", path);
            return 2;
        }
        return grep_dir(path, g);
    }

    if (!S_ISREG(st.st_mode))
        return 1; /* skip special files */

    /* Apply glob filters on basename */
    if (glob_excluded(path_basename(path), g))
        return 1;

    /* --vcs: skip hidden files and noise extensions */
    if (g->opt_vcs) {
        const char *base = path_basename(path);
        if (base[0] == '.') return 1;  /* hidden */
        if (vcsignore_skip_name(base, 0)) return 1;
        /* Check .gitignore rules against basename */
        if (g->vcs_ign && vcsignore_match(g->vcs_ign, base, 0)) return 1;
    }

    /* --vcs: silently skip binary files (null-byte check in first 512 bytes).
     * Use raw read(2) before fopen to avoid stdio buffering interactions. */
    if (g->opt_vcs) {
        int bfd = open(path, O_RDONLY);
        if (bfd >= 0) {
            char probe[512];
            ssize_t nr = read(bfd, probe, sizeof(probe));
            close(bfd);
            if (nr > 0 && memchr(probe, '\0', (size_t)nr))
                return 1; /* binary: skip silently */
        }
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (!g->opt_s)
            err_sys("grep", "%s", path);
        return 2;
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL); /* advisory */

    int r = grep_stream(fd, path, show_fname, g, 0);
    close(fd);
    /* -L: print filename of files with NO match */
    if (g->opt_L && r == 1) {
        if (g->opt_Z) {
            printf("%s%c", path, '\0');
        } else {
            printf("%s\n", path);
        }
        return 1; /* still "no match" for exit status */
    }
    return r;
}

/* Load patterns from a file (-f FILE). */
static int load_pattern_file(const char *fname, grep_opts_t *g)
{
    FILE *fp = fopen(fname, "r");
    if (!fp) {
        err_sys("grep", "%s", fname);
        return 1;
    }

    char   *line    = NULL;
    size_t  cap     = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, fp)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (g->npatterns >= MAX_PATTERNS) {
            err_msg("grep", "too many patterns (max %d)", MAX_PATTERNS);
            free(line);
            fclose(fp);
            return 1;
        }
        g->patterns[g->npatterns] = strdup(line);
        if (!g->patterns[g->npatterns]) {
            err_msg("grep", "out of memory");
            free(line);
            fclose(fp);
            return 1;
        }
        g->npatterns++;
    }

    free(line);
    fclose(fp);
    return 0;
}

/* Free all resources in grep_opts_t */
/* Idempotent: resets npatterns so a second call (e.g. an error path that also
 * frees, then falls through to the cleanup label) cannot double-free. */
static void grep_opts_free(grep_opts_t *g)
{
    for (int i = 0; i < g->npatterns; i++) {
        free(g->patterns[i]);
        g->patterns[i] = NULL;
    }
    g->npatterns = 0;
    if (g->re) { mb_regex_free(g->re); g->re = NULL; }
    /* The input buffer is kept for the next run (inside `silex sh` grep is
     * a function call, often a short one): unless a long line grew it. */
    if (g->rbuf && g->rcap <= GREP_KEEP_BUF) {
        g_rbuf = g->rbuf;
        g_rcap = g->rcap;
    } else {
        free(g->rbuf);
    }
    g->rbuf = NULL;
    g->rcap = 0;
    if (g->vcs_ign) { vcsignore_free(g->vcs_ign); g->vcs_ign = NULL; }
}

/* ------------------------------------------------------------------ */
/* Main applet entry                                                   */
/* ------------------------------------------------------------------ */

int applet_grep(int argc, char **argv)
{
    grep_opts_t g;
    memset(&g, 0, sizeof(g));
    /* Process-wide result of the PREVIOUS invocation otherwise: inside
     * `silex sh` an applet is a function call, so a `grep -q` that matched
     * once made every later `grep -q` in the same shell exit 0. */
    g_matched_any = 0;
    g_ctx_output = 0;
    g.rbuf = g_rbuf;
    g.rcap = g_rcap;
    g_rbuf = NULL;
    g_rcap = 0;

    /* SILEX_SMART=1: enable smart-case and VCS-aware mode by default.
     * Read getenv once: a second call is not guaranteed to return the same
     * (non-NULL) pointer, so strcmp() could be handed NULL. */
    const char *smart_env = getenv("SILEX_SMART");
    if (smart_env && strcmp(smart_env, "1") == 0)
        g.opt_smart = 1;

    /* --color: only if stdout is a tty */
    int color_auto = isatty(STDOUT_FILENO);

    /* Explicit 128KB stdout buffer when not a tty: reduces write() count ~30x
     * (1365 → ~45 for 100k matching lines) without buffering interactive output. */
    if (!color_auto) {
        static char grep_out_buf[131072];
        setvbuf(stdout, grep_out_buf, _IOFBF, sizeof(grep_out_buf));
    }

    enum {
        L_VCS = 0x100, L_COLOR, L_INCLUDE, L_EXCLUDE, L_REGEXP, L_FILE
    };
    static const opt_long_t longs[] = {
        { "extended-regexp",    OPT_BARE,              'E'       },
        { "fixed-strings",      OPT_BARE,              'F'       },
        { "basic-regexp",       OPT_BARE,              'G'       },
        { "count",              OPT_BARE,              'c'       },
        { "ignore-case",        OPT_BARE,              'i'       },
        { "files-with-matches", OPT_BARE,              'l'       },
        { "line-number",        OPT_BARE,              'n'       },
        { "quiet",              OPT_BARE,              'q'       },
        { "silent",             OPT_BARE,              'q'       },
        { "no-messages",        OPT_BARE,              's'       },
        { "invert-match",       OPT_BARE,              'v'       },
        { "word-regexp",        OPT_BARE,              'w'       },
        { "recursive",          OPT_BARE,              'r'       },
        { "vcs",                OPT_BARE,              L_VCS     },
        { "smart-case",         OPT_BARE,              'S'       },
        { "color",              OPT_BARE | OPT_JOINED, L_COLOR   },
        { "colour",             OPT_BARE | OPT_JOINED, L_COLOR   },
        { "include",            OPT_REQ,               L_INCLUDE },
        { "exclude",            OPT_REQ,               L_EXCLUDE },
        { "regexp",             OPT_NEXT,              L_REGEXP  },
        { "file",               OPT_NEXT,              L_FILE    },
        { NULL, 0, 0 }
    };
    opt_t o;
    int c;

    opt_init(&o, argc, argv, "EFGcilLHhnqsvwrRoxbZSm:A:B:C:e:f:", longs, OPT_GNU);
    while ((c = opt_next(&o)) != OPT_DONE) {
        switch (c) {
        case 'E': g.opt_E = 1; break;
        case 'F': g.opt_F = 1; break;
        case 'G': g.opt_G = 1; break;
        case 'c': g.opt_c = 1; break;
        case 'i': g.opt_i = 1; break;
        case 'l': g.opt_l = 1; break;
        case 'L': g.opt_L = 1; break;
        case 'H': g.opt_H = 1; break;
        case 'h': g.opt_h = 1; break;
        case 'n': g.opt_n = 1; break;
        case 'q': g.opt_q = 1; break;
        case 's': g.opt_s = 1; break;
        case 'v': g.opt_v = 1; break;
        case 'w': g.opt_w = 1; break;
        case 'r': case 'R': g.opt_r = 1; break;
        case 'o': g.opt_o = 1; break;
        case 'x': g.opt_x = 1; break;
        case 'b': g.opt_b = 1; break;
        case 'Z': g.opt_Z = 1; break;
        case 'S': g.opt_smart = 1; break;
        case L_VCS: g.opt_vcs = 1; g.opt_r = 1; break;
        case L_COLOR:
            if (!o.arg || strcmp(o.arg, "always") == 0)
                g.opt_color = 1;
            else if (strcmp(o.arg, "auto") == 0)
                g.opt_color = color_auto;
            else if (strcmp(o.arg, "never") == 0)
                g.opt_color = 0;
            else
                goto unknown;
            break;
        case L_INCLUDE:
            if (g.n_include >= MAX_GLOBS) { err_msg("grep", "too many --include globs"); goto parse_error; }
            g.include_globs[g.n_include++] = (char *)o.arg;
            break;
        case L_EXCLUDE:
            if (g.n_exclude >= MAX_GLOBS) { err_msg("grep", "too many --exclude globs"); goto parse_error; }
            g.exclude_globs[g.n_exclude++] = (char *)o.arg;
            break;
        case 'm': g.max_matches = atoi(o.arg); g.max_given = 1; break;
        case 'A': g.context_A = atoi(o.arg); break;
        case 'B': g.context_B = atoi(o.arg); break;
        case 'C': g.context_A = g.context_B = atoi(o.arg); break;
        case 'e':
        case L_REGEXP:
            g.pattern_given = 1;
            if (g.npatterns >= MAX_PATTERNS) {
                err_msg("grep", "too many patterns");
                goto parse_error;
            }
            g.patterns[g.npatterns] = strdup(o.arg);
            if (!g.patterns[g.npatterns]) {
                err_msg("grep", "out of memory");
                goto parse_error;
            }
            g.npatterns++;
            break;
        case 'f':
        case L_FILE:
            g.pattern_given = 1;
            if (load_pattern_file(o.arg, &g) != 0)
                goto parse_error;
            break;
        case OPT_MISSING:
            if (o.islong)
                err_msg("grep", "--%s requires argument", o.lopt->name);
            else
                err_msg("grep", "-%c requires argument", o.opt);
            goto parse_error;
        default:
        unknown:
            /* Unknown long option: try module lookup */
            if (o.islong) {
                silex_module_t *mod = registry_lookup("grep", o.cur);
                if (mod) {
                    grep_opts_free(&g);
                    return mod->handler(argc, argv, o.cur_ind);
                }
                err_msg("grep", "unrecognized option '%s'", o.cur);
                goto parse_error;
            }
            /* Try module lookup before error */
            {
                char flag_str[3] = {'-', (char)o.opt, '\0'};
                silex_module_t *mod = registry_lookup("grep", flag_str);
                if (mod) {
                    grep_opts_free(&g);
                    return mod->handler(argc, argv, o.cur_ind);
                }
            }
            err_msg("grep", "unrecognized option '-%c'", o.opt);
            goto parse_error;
        }
    }
    int i = o.ind;

    /* -G names the default matcher; together with -E or -F it is a
     * conflict, as in GNU grep. */
    if (g.opt_G && (g.opt_E || g.opt_F)) {
        err_msg("grep", "conflicting matchers specified");
        goto parse_error;
    }

    /* If no -e/-f patterns, first non-option arg is the pattern. An -f
     * file with no lines gives no patterns at all: nothing matches. */
    if (g.npatterns == 0 && !g.pattern_given) {
        if (i >= argc) {
            err_usage("grep", "[-EFciln qsvwrR] [-e PAT] [-f FILE] PATTERN [FILE...]");
            goto parse_error;
        }
        g.patterns[0] = strdup(argv[i]);
        if (!g.patterns[0]) {
            err_msg("grep", "out of memory");
            goto parse_error;
        }
        g.npatterns = 1;
        i++;
    }

    /* SILEX_SMART: when smart mode is active and recursive, enable --vcs */
    if (g.opt_smart && g.opt_r && !g.opt_vcs)
        g.opt_vcs = 1;

    /* Load VCS ignore rules when --vcs.
     * Use the first directory argument as the root, falling back to ".". */
    if (g.opt_vcs) {
        const char *vcs_root = ".";
        for (int k = i; k < argc; k++) {
            struct stat vst;
            if (stat(argv[k], &vst) == 0 && S_ISDIR(vst.st_mode)) {
                vcs_root = argv[k];
                break;
            }
        }
        g.vcs_ign = vcsignore_load(vcs_root);
    }

    if (compile_patterns(&g) != 0) {
        grep_opts_free(&g);
        goto parse_error;
    }

    /* GNU quits without reading any input when nothing can be selected:
     * -m 0, or -v with only empty patterns (which match every line), or no
     * patterns at all (-f /dev/null) without -v. No output, not even -c's
     * count, and no diagnostics for unreadable files. Not for -L, which
     * lists exactly the files where nothing was selected. */
    if (!g.opt_L) {
        int all_empty = 1;
        for (int k = 0; k < g.npatterns && all_empty; k++)
            if (g.patterns[k][strspn(g.patterns[k], "\n")] != '\0') all_empty = 0;
        int nokeys = g.npatterns == 0;
        int inv = g.opt_v ^ nokeys;
        if ((g.max_given && g.max_matches == 0) ||
            (all_empty && inv && (nokeys || (!g.opt_x && !g.opt_w)))) {
            grep_opts_free(&g);
            return 1;
        }
    }

    g.all_lines = mb_regex_all_lines(g.re);

    /* Output to /dev/null (GNU's test: same inode as /dev/null) */
    {
        struct stat so, sn;
        g.devnull = fstat(STDOUT_FILENO, &so) == 0 && S_ISCHR(so.st_mode) &&
                    stat("/dev/null", &sn) == 0 &&
                    so.st_ino == sn.st_ino && so.st_dev == sn.st_dev;
    }

    int final_result = 1; /* no match yet */
    /* An error is remembered SEPARATELY from the match result. A match used to
     * overwrite it (`r == 2 && final_result != 0`), so `grep pat missing found`
     * printed "No such file or directory" and still exited 0 -- a script
     * checking grep's status could not tell a clean search from one that never
     * opened half its files. GNU: any error is exit 2, even with matches. */
    int had_error = 0;
    int nfiles = argc - i;

    if (nfiles == 0) {
        /* Read from stdin */
        /* named "(standard input)" for -H, -l, -L and errors, as in GNU */
        int r = grep_stream(STDIN_FILENO, "(standard input)",
                            g.opt_H && !g.opt_h, &g, 1);
        if (g.opt_L && r == 1) {
            if (g.opt_Z) {
                fputs("(standard input)", stdout);
                putc('\0', stdout);
            } else {
                puts("(standard input)");
            }
        }
        if (r == 0)
            final_result = 0;
        else if (r == 2)
            had_error = 1;
    } else {
        int show_fname = (nfiles > 1) || g.opt_r || g.opt_H;
        if (g.opt_h) show_fname = 0;  /* -h overrides */
        for (int j = i; j < argc; j++) {
            int r = grep_path(argv[j], show_fname, 0, &g);
            if (r == 0)
                final_result = 0;
            else if (r == 2)
                had_error = 1;
        }
    }

    grep_opts_free(&g);

    /* -q reports only whether something matched: POSIX says it exits 0 on a
     * match even when a file could not be read. */
    if (g.opt_q && g_matched_any)
        return 0;
    return had_error ? 2 : final_result;

parse_error:
    /* Every option-parsing error jumps here so the strdup'd pattern list is
     * freed before we exit. grep runs in-process as a builtin, so leaking it
     * would accumulate across a shell session. grep_opts_free is idempotent,
     * so paths that already freed can still fall through safely. */
    grep_opts_free(&g);
    return 2;
}
