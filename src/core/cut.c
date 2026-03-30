/* cut.c -- cut builtin: remove sections from each line of files */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* cut.c — cut builtin */

#include "../util/error.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Range list                                                            */
/* ------------------------------------------------------------------ */

/*
 * A single range entry.  lo and hi are 1-based field/byte numbers.
 * hi == INT_MAX means "to end of line".
 * lo == 0 means "from start" (i.e. -N form).
 */
typedef struct {
    int lo;
    int hi;
} range_t;

#define MAX_RANGES 512

typedef struct {
    range_t r[MAX_RANGES];
    int     n;
} rangelist_t;

/*
 * Parse a position list string like "1,3-5,7-" into rl.
 * Returns 0 on success, -1 on error.
 */
static int parse_positions(const char *s, rangelist_t *rl)
{
    rl->n = 0;
    const char *p = s;

    while (*p) {
        /* Find end of this token (up to next ',') */
        const char *comma = strchr(p, ',');
        size_t toklen = comma ? (size_t)(comma - p) : strlen(p);

        char tok[64];
        if (toklen == 0 || toklen >= sizeof(tok)) {
            err_msg("cut", "invalid range '%.*s'", (int)toklen, p);
            return -1;
        }
        memcpy(tok, p, toklen);
        tok[toklen] = '\0';

        if (rl->n >= MAX_RANGES) {
            err_msg("cut", "too many ranges");
            return -1;
        }

        range_t *r = &rl->r[rl->n];

        /* Forms: N  |  N-M  |  N-  |  -M */
        char *dash = strchr(tok, '-');
        if (!dash) {
            /* Single position N */
            char *endp;
            long v = strtol(tok, &endp, 10);
            if (*endp != '\0' || v < 1) {
                err_msg("cut", "invalid field value '%s'", tok);
                return -1;
            }
            r->lo = r->hi = (int)v;
        } else if (dash == tok) {
            /* -M form: from 1 to M */
            char *endp;
            long v = strtol(tok + 1, &endp, 10);
            if (*endp != '\0' || v < 1) {
                err_msg("cut", "invalid field value '%s'", tok);
                return -1;
            }
            r->lo = 1;
            r->hi = (int)v;
        } else {
            /* N- or N-M */
            *dash = '\0';
            char *endp;
            long lo = strtol(tok, &endp, 10);
            if (*endp != '\0' || lo < 1) {
                err_msg("cut", "invalid field value '%s'", tok);
                return -1;
            }
            r->lo = (int)lo;
            if (*(dash + 1) == '\0') {
                /* N- form */
                r->hi = INT_MAX;
            } else {
                long hi = strtol(dash + 1, &endp, 10);
                if (*endp != '\0' || hi < 1) {
                    err_msg("cut", "invalid field value '%s'", tok);
                    return -1;
                }
                if (hi < lo) {
                    err_msg("cut", "invalid decreasing range");
                    return -1;
                }
                r->hi = (int)hi;
            }
        }

        rl->n++;
        p += toklen;
        if (*p == ',') p++;
    }

    if (rl->n == 0) {
        err_msg("cut", "no positions specified");
        return -1;
    }

    return 0;
}

/*
 * Return 1 if position pos (1-based) is selected by rl.
 */
static int in_range(const rangelist_t *rl, int pos)
{
    for (int i = 0; i < rl->n; i++) {
        if (pos >= rl->r[i].lo && pos <= rl->r[i].hi)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Line processing                                                       */
/* ------------------------------------------------------------------ */

/*
 * Process one line for byte/char mode (-b/-c).
 * Line does NOT include the newline terminator.
 * Outputs selected bytes, then a newline.
 */
static void cut_bytes(const char *line, size_t len, const rangelist_t *rl)
{
    for (size_t i = 0; i < len; i++) {
        int pos = (int)(i + 1);
        if (in_range(rl, pos))
            putchar((unsigned char)line[i]);
    }
    putchar('\n');
}

/*
 * Process one line for field mode (-f).
 * delim is the single-byte field delimiter.
 * suppress: if set, lines without delimiter are not printed.
 * Outputs selected fields joined by delim, then newline.
 */
static void cut_fields(const char *line, size_t len,
                        const rangelist_t *rl, char delim, int suppress)
{
    /* Check if delimiter is present */
    int has_delim = (memchr(line, (unsigned char)delim, len) != NULL);

    if (!has_delim) {
        /* No delimiter in line */
        if (!suppress) {
            fwrite(line, 1, len, stdout);
            putchar('\n');
        }
        return;
    }

    /*
     * Split line into fields and output selected ones.
     * We collect pointers to field starts and their lengths.
     */
    const char *p    = line;
    const char *end  = line + len;
    int   field      = 1;
    int   first_out  = 1; /* used to know when to print delimiter */

    while (p <= end) {
        /* Find end of this field */
        const char *next = memchr(p, (unsigned char)delim, (size_t)(end - p));
        size_t flen;
        if (next) {
            flen = (size_t)(next - p);
        } else {
            flen = (size_t)(end - p);
            next = end;
        }

        if (in_range(rl, field)) {
            if (!first_out) putchar(delim);
            fwrite(p, 1, flen, stdout);
            first_out = 0;
        }

        field++;
        p = next + 1;
        if (next == end) break;
    }

    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */

int applet_cut(int argc, char **argv)
{
    int opt_b = 0; /* -b: byte positions */
    int opt_c = 0; /* -c: character positions (treated as bytes) */
    int opt_f = 0; /* -f: fields */
    int opt_s = 0; /* -s: suppress lines without delimiter */
    char delim = '\t';
    int have_delim = 0;
    const char *positions = NULL;
    int mode = 0; /* 'b', 'c', or 'f' */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strncmp(arg, "--bytes=", 8) == 0) {
            positions = arg + 8; opt_b = 1; mode = 'b'; continue;
        }
        if (strncmp(arg, "--characters=", 13) == 0) {
            positions = arg + 13; opt_c = 1; mode = 'c'; continue;
        }
        if (strncmp(arg, "--fields=", 9) == 0) {
            positions = arg + 9; opt_f = 1; mode = 'f'; continue;
        }
        if (strncmp(arg, "--delimiter=", 12) == 0) {
            if (strlen(arg + 12) != 1) {
                err_msg("cut", "the delimiter must be a single character");
                return 1;
            }
            delim = arg[12]; have_delim = 1; continue;
        }
        if (strcmp(arg, "--only-delimited") == 0) { opt_s = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'b':
                opt_b = 1; mode = 'b';
                if (p[1]) { positions = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("cut", "-b LIST [-z] [FILE...]");
                        return 1;
                    }
                    positions = argv[i];
                }
                break;
            case 'c':
                opt_c = 1; mode = 'c';
                if (p[1]) { positions = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("cut", "-c LIST [-z] [FILE...]");
                        return 1;
                    }
                    positions = argv[i];
                }
                break;
            case 'f':
                opt_f = 1; mode = 'f';
                if (p[1]) { positions = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("cut", "-f LIST [-d DELIM] [-s] [FILE...]");
                        return 1;
                    }
                    positions = argv[i];
                }
                break;
            case 'd':
                if (p[1]) {
                    if (p[2] != '\0') {
                        err_msg("cut", "the delimiter must be a single character");
                        return 1;
                    }
                    delim = p[1];
                    have_delim = 1;
                    stop = 1;
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("cut", "-f LIST -d DELIM [FILE...]");
                        return 1;
                    }
                    if (strlen(argv[i]) != 1) {
                        err_msg("cut", "the delimiter must be a single character");
                        return 1;
                    }
                    delim = argv[i][0];
                    have_delim = 1;
                }
                break;
            case 's': opt_s = 1; break;
            default:
                err_msg("cut", "unrecognized option '-%c'", *p);
                err_usage("cut", "[-bcf] LIST [-d DELIM] [-s] [FILE...]");
                return 1;
            }
            p++;
        }
    }

    if (mode == 0) {
        err_msg("cut", "you must specify a list of bytes, characters, or fields");
        err_usage("cut", "[-bcf] LIST [-d DELIM] [-s] [FILE...]");
        return 1;
    }

    if (!positions) {
        err_msg("cut", "missing position list");
        return 1;
    }

    /* -d and -s are only meaningful with -f */
    if (have_delim && !opt_f) {
        err_msg("cut", "an input delimiter may only be specified when operating on fields");
        return 1;
    }
    if (opt_s && !opt_f) {
        err_msg("cut", "-s requires -f");
        return 1;
    }

    (void)opt_b; (void)opt_c; (void)opt_f;

    rangelist_t rl;
    if (parse_positions(positions, &rl) != 0)
        return 1;

    /*
     * Process files (or stdin if no file args).
     * We read line-by-line using a dynamic buffer to handle long lines.
     */
    int file_start = i;
    int ret = 0;

    /* We process either file list or stdin */
    int do_stdin = (file_start >= argc);
    int file_idx = file_start;

    for (;;) {
        FILE *fp;
        if (do_stdin) {
            fp = stdin;
        } else {
            if (file_idx >= argc) break;
            const char *fname = argv[file_idx++];
            if (strcmp(fname, "-") == 0) {
                fp = stdin;
            } else {
                fp = fopen(fname, "r");
                if (!fp) {
                    err_sys("cut", "cannot open '%s'", fname);
                    ret = 1;
                    continue;
                }
            }
        }

        /* Read line by line */
        char   *linebuf = NULL;
        size_t  linecap = 0;
        ssize_t linelen;

        while ((linelen = getline(&linebuf, &linecap, fp)) >= 0) {
            /* Strip trailing newline */
            size_t len = (size_t)linelen;
            if (len > 0 && linebuf[len - 1] == '\n') {
                linebuf[--len] = '\0';
                /* Also strip \r for CRLF */
                if (len > 0 && linebuf[len - 1] == '\r')
                    linebuf[--len] = '\0';
            }

            if (mode == 'b' || mode == 'c') {
                cut_bytes(linebuf, len, &rl);
            } else {
                cut_fields(linebuf, len, &rl, delim, opt_s);
            }
        }

        free(linebuf);

        if (fp != stdin) fclose(fp);
        if (do_stdin) break;
    }

    return ret;
}
