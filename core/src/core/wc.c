/* wc.c — wc builtin: print newline, word, and byte counts for each file */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/linescan.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Per-file statistics */
typedef struct {
    long long lines;
    long long words;
    long long bytes;
    long long chars;   /* same as bytes for now (no wide-char) */
    long long maxline; /* max line length (-L) */
} wc_counts_t;

/* Read buffer size for wc — 256KB matches GNU wc's read chunk size,
 * cutting read() syscall count by 4x vs the previous 64KB buffer. */
#define WC_BUF_SIZE 262144

/*
 * Count statistics for stream fp.
 * Uses fread() into a buffer and scan_newline() for the line-count hot path.
 *
 * need_words: 1 when word/maxline counting is also required.
 * When need_words == 0, the inner per-byte loop is skipped entirely: we
 * only call scan_newline() to jump between newlines, counting them as we go.
 * This matches GNU wc's performance for "wc -l" (line-count-only) workloads.
 */
static int wc_count(FILE *fp, wc_counts_t *c, int need_words)
{
    static char buf[WC_BUF_SIZE];  /* static: avoids large stack alloc */
    memset(c, 0, sizeof(*c));

    long long cur_line_len = 0;
    int in_word = 0;

    for (;;) {
        size_t nr = fread(buf, 1, sizeof(buf), fp);
        if (nr == 0)
            break;

        c->bytes += (long long)nr;
        c->chars += (long long)nr;

        const char *p   = buf;
        const char *end = buf + nr;

        if (!need_words) {
            /*
             * Fast path: line-count (and byte-count) only.
             * Skip the per-byte word/maxline loop; use scan_newline() to
             * jump directly to each '\n', increment lines, and continue.
             */
            while (p < end) {
                const char *nl = scan_newline(p, (size_t)(end - p));
                if (nl < end) {
                    c->lines++;
                    p = nl + 1;
                } else {
                    break;
                }
            }
        } else {
            while (p < end) {
                /* Jump to the next newline using vectorised scan */
                const char *nl = scan_newline(p, (size_t)(end - p));

                /* Process bytes between p and nl (none contain '\n') */
                while (p < nl) {
                    unsigned char ch = (unsigned char)*p++;
                    cur_line_len++;
                    if (isspace(ch)) {
                        in_word = 0;
                    } else {
                        if (!in_word) { c->words++; in_word = 1; }
                    }
                }

                if (p < end) {
                    /* *p == '\n' */
                    c->lines++;
                    if (cur_line_len > c->maxline)
                        c->maxline = cur_line_len;
                    cur_line_len = 0;
                    in_word = 0;
                    p++;   /* skip the newline */
                }
            }
        }
    }

    /* Handle file not ending with newline: update maxline */
    if (cur_line_len > c->maxline)
        c->maxline = cur_line_len;

    return ferror(fp) ? 1 : 0;
}

/*
 * Print one result line.
 * width: minimum field width for right-aligned counts.
 * flags control which fields to print.
 */
static void wc_print(const wc_counts_t *c, int do_l, int do_w, int do_c,
                     int do_m, int do_L, int width, const char *label)
{
    /* Build format string dynamically to right-align each field */
    int first = 1;

#define PRINT_FIELD(val) \
    do { \
        if (!first) fputc(' ', stdout); \
        fprintf(stdout, "%*lld", width, (long long)(val)); \
        first = 0; \
    } while (0)

    if (do_l) PRINT_FIELD(c->lines);
    if (do_w) PRINT_FIELD(c->words);
    if (do_c) PRINT_FIELD(c->bytes);
    if (do_m) PRINT_FIELD(c->chars);
    if (do_L) PRINT_FIELD(c->maxline);

#undef PRINT_FIELD

    if (label && *label)
        fprintf(stdout, " %s", label);
    fputc('\n', stdout);
}

/*
 * Determine the minimum field width needed to display val.
 * (number of decimal digits)
 */
static int count_digits(long long val)
{
    if (val < 0) val = -val;
    if (val == 0) return 1;
    int d = 0;
    while (val > 0) { d++; val /= 10; }
    return d;
}

int applet_wc(int argc, char **argv)
{
    int opt_l = 0;   /* -l lines */
    int opt_w = 0;   /* -w words */
    int opt_c = 0;   /* -c bytes */
    int opt_m = 0;   /* -m chars */
    int opt_L = 0;   /* -L max line length */
    int ret   = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        if (strcmp(arg, "--lines") == 0)     { opt_l = 1; continue; }
        if (strcmp(arg, "--words") == 0)     { opt_w = 1; continue; }
        if (strcmp(arg, "--bytes") == 0)     { opt_c = 1; continue; }
        if (strcmp(arg, "--chars") == 0)     { opt_m = 1; continue; }
        if (strcmp(arg, "--max-line-length") == 0) { opt_L = 1; continue; }

        const char *p = arg + 1;
        int unknown = 0;
        while (*p && !unknown) {
            switch (*p) {
            case 'l': opt_l = 1; break;
            case 'w': opt_w = 1; break;
            case 'c': opt_c = 1; break;
            case 'm': opt_m = 1; break;
            case 'L': opt_L = 1; break;
            default:
                err_msg("wc", "unrecognized option '-%c'", *p);
                err_usage("wc", "[-clwmL] [FILE...]");
                return 1;
            }
            p++;
        }
        if (unknown) return 1;
    }

    /* Default: lines, words, bytes */
    int default_mode = !(opt_l || opt_w || opt_c || opt_m || opt_L);
    if (default_mode) {
        opt_l = opt_w = opt_c = 1;
    }

    /* need_words: 1 if any of word/maxline/char counting is requested */
    int need_words = (opt_w || opt_m || opt_L);

    int nfiles = argc - i;

    if (nfiles == 0) {
        /* stdin only */
        wc_counts_t c;
        if (wc_count(stdin, &c, need_words) != 0) {
            err_sys("wc", "read error");
            ret = 1;
        }
        /* Determine display width from the largest value we'll print */
        long long maxval = 0;
        if (opt_l && c.lines   > maxval) maxval = c.lines;
        if (opt_w && c.words   > maxval) maxval = c.words;
        if (opt_c && c.bytes   > maxval) maxval = c.bytes;
        if (opt_m && c.chars   > maxval) maxval = c.chars;
        if (opt_L && c.maxline > maxval) maxval = c.maxline;
        int width = count_digits(maxval);
        if (width < 1) width = 1;

        wc_print(&c, opt_l, opt_w, opt_c, opt_m, opt_L, width, "");
        return ret;
    }

    wc_counts_t *counts = calloc((size_t)nfiles, sizeof(wc_counts_t));
    if (!counts) { err_msg("wc", "out of memory"); return 1; }

    for (int j = 0; j < nfiles; j++) {
        const char *fname = argv[i + j];
        FILE *fp;
        if (strcmp(fname, "-") == 0) {
            fp = stdin;
        } else {
            fp = fopen(fname, "r");
            if (!fp) {
                err_sys("wc", "cannot open '%s'", fname);
                ret = 1;
                continue;
            }
            posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL);
        }
        if (wc_count(fp, &counts[j], need_words) != 0) {
            err_sys("wc", "read error on '%s'", fname);
            ret = 1;
        }
        if (fp != stdin) fclose(fp);
    }

    /* Compute total and maximum value for column-width determination */
    wc_counts_t total = {0, 0, 0, 0, 0};
    long long maxval = 0;

    for (int j = 0; j < nfiles; j++) {
        total.lines   += counts[j].lines;
        total.words   += counts[j].words;
        total.bytes   += counts[j].bytes;
        total.chars   += counts[j].chars;
        if (counts[j].maxline > total.maxline) total.maxline = counts[j].maxline;

        if (opt_l && counts[j].lines   > maxval) maxval = counts[j].lines;
        if (opt_w && counts[j].words   > maxval) maxval = counts[j].words;
        if (opt_c && counts[j].bytes   > maxval) maxval = counts[j].bytes;
        if (opt_m && counts[j].chars   > maxval) maxval = counts[j].chars;
        if (opt_L && counts[j].maxline > maxval) maxval = counts[j].maxline;
        /* For multi-file output, factor in byte count for column-width even
         * when -c was not requested (matches coreutils/uutils behaviour). */
        if (nfiles > 1 && counts[j].bytes > maxval) maxval = counts[j].bytes;
    }

    if (nfiles > 1) {
        if (opt_l && total.lines   > maxval) maxval = total.lines;
        if (opt_w && total.words   > maxval) maxval = total.words;
        if (opt_c && total.bytes   > maxval) maxval = total.bytes;
        if (opt_m && total.chars   > maxval) maxval = total.chars;
        if (opt_L && total.maxline > maxval) maxval = total.maxline;
        if (total.bytes > maxval) maxval = total.bytes;
    }

    int width = count_digits(maxval);
    if (width < 1) width = 1;

    for (int j = 0; j < nfiles; j++) {
        const char *fname = argv[i + j];
        const char *label = (strcmp(fname, "-") == 0) ? "" : fname;
        wc_print(&counts[j], opt_l, opt_w, opt_c, opt_m, opt_L, width, label);
    }

    if (nfiles > 1) {
        wc_print(&total, opt_l, opt_w, opt_c, opt_m, opt_L, width, "total");
    }

    free(counts);
    return ret;
}
