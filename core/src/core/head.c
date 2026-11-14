/* head.c — head builtin: output the first part of files */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/opt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Parse a count argument for -n or -c.
 * Accepts:  N   (positive: print first N)
 *           -N  (negative: print all but last N)
 *
 * On success stores the absolute value in *val and sets *negative to 1 if
 * the leading '-' was present.  Returns 0 on success, -1 on error.
 */
static int parse_count(const char *s, long long *val, int *negative)
{
    *negative = 0;
    if (*s == '-') {
        *negative = 1;
        s++;
    }
    if (*s == '\0') return -1;

    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (*end != '\0' || errno != 0 || v < 0) return -1;
    *val = v;
    return 0;
}

/* ---- line-mode helpers --------------------------------------------------- */

/*
 * Print first n lines of stream fp to stdout.
 * Returns 0 on success, 1 on I/O error.
 */
static int head_lines_first(FILE *fp, long long n)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long long count = 0;

    while (count < n && (len = getline(&line, &cap, fp)) != -1) {
        if (fwrite(line, 1, (size_t)len, stdout) != (size_t)len) {
            free(line);
            return 1;
        }
        count++;
    }
    free(line);
    if (ferror(fp)) return 1;
    return 0;
}

/*
 * Print all but last n lines of stream fp to stdout.
 * Uses a circular buffer of n line strings.
 * Returns 0 on success, 1 on error.
 */
static int head_lines_but_last(FILE *fp, long long n)
{
    if (n == 0) {
        /* Print everything */
        char buf[8192];
        size_t nr;
        while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) {
            if (fwrite(buf, 1, nr, stdout) != nr) return 1;
        }
        return ferror(fp) ? 1 : 0;
    }

    /* Circular buffer of n line pointers */
    char **ring = calloc((size_t)n, sizeof(char *));
    if (!ring) {
        err_msg("head", "out of memory");
        return 1;
    }

    char *raw = NULL;
    size_t cap = 0;
    ssize_t len;
    long long idx = 0;   /* index into ring (mod n) */
    long long total = 0; /* total lines read */

    while ((len = getline(&raw, &cap, fp)) != -1) {
        long long slot = idx % n;

        /* Slot to evict: print it if the ring is full */
        if (total >= n) {
            const char *old = ring[slot];
            size_t olen = strlen(old);
            if (fwrite(old, 1, olen, stdout) != olen) goto fail;
            free(ring[slot]);
            ring[slot] = NULL;
        }

        ring[slot] = malloc((size_t)len + 1);
        if (!ring[slot]) {
            err_msg("head", "out of memory");
            goto fail;
        }
        memcpy(ring[slot], raw, (size_t)len);
        ring[slot][len] = '\0';

        idx++;
        total++;
    }

    /* NULL it: the fail path below frees raw again, and `goto fail` on the very
     * next line would otherwise double-free. free(NULL) is a no-op. */
    free(raw);
    raw = NULL;
    if (ferror(fp)) goto fail;

    /* Free remaining ring entries (last n lines — do not print) */
    for (long long j = 0; j < n; j++) {
        free(ring[j]);
        ring[j] = NULL;
    }
    free(ring);
    return 0;

fail:
    free(raw);
    for (long long j = 0; j < n; j++) free(ring[j]);
    free(ring);
    return 1;
}

/* ---- byte-mode helpers --------------------------------------------------- */

/*
 * Print first n bytes of fp.
 */
static int head_bytes_first(FILE *fp, long long n)
{
    char buf[8192];
    long long remaining = n;
    while (remaining > 0) {
        size_t want = (remaining > (long long)sizeof(buf))
                      ? sizeof(buf) : (size_t)remaining;
        size_t nr = fread(buf, 1, want, fp);
        if (nr == 0) break;
        if (fwrite(buf, 1, nr, stdout) != nr) return 1;
        remaining -= (long long)nr;
    }
    return ferror(fp) ? 1 : 0;
}

/*
 * Print all but last n bytes of fp.
 * Buffers the entire file into memory.
 */
static int head_bytes_but_last(FILE *fp, long long n)
{
    /* Read entire file */
    size_t used = 0, cap = 65536;
    char *data = malloc(cap);
    if (!data) { err_msg("head", "out of memory"); return 1; }

    for (;;) {
        if (used == cap) {
            size_t newcap = cap * 2;
            if (newcap < cap) { free(data); err_msg("head", "file too large"); return 1; }
            char *tmp = realloc(data, newcap);
            if (!tmp) { free(data); err_msg("head", "out of memory"); return 1; }
            data = tmp;
            cap = newcap;
        }
        size_t nr = fread(data + used, 1, cap - used, fp);
        if (nr == 0) break;
        used += nr;
    }
    if (ferror(fp)) { free(data); return 1; }

    long long print_bytes = (long long)used - n;
    if (print_bytes > 0) {
        if (fwrite(data, 1, (size_t)print_bytes, stdout) != (size_t)print_bytes) {
            free(data);
            return 1;
        }
    }
    free(data);
    return 0;
}

/* ---- per-file driver ------------------------------------------------------ */

static int head_file(const char *filename, FILE *fp,
                     int use_bytes, long long count, int negative)
{
    if (use_bytes) {
        if (negative)
            return head_bytes_but_last(fp, count);
        else
            return head_bytes_first(fp, count);
    } else {
        if (negative)
            return head_lines_but_last(fp, count);
        else
            return head_lines_first(fp, count);
    }
    (void)filename;
}

/* ---- applet entry point -------------------------------------------------- */

int applet_head(int argc, char **argv)
{
    long long count = 10;
    int negative  = 0;   /* all-but-last mode */
    int use_bytes = 0;
    int opt_q     = 0;
    int opt_v     = 0;
    int ret       = 0;
    static const opt_long_t longs[] = {
        { "quiet",   OPT_BARE, 'q' },
        { "silent",  OPT_BARE, 'q' },
        { "verbose", OPT_BARE, 'v' },
        /* Long --lines/--bytes, joined (`--bytes=4`) or separated
         * (`--bytes 4`) -- getopt_long accepts both for a required
         * argument, and only the joined form used to be handled. */
        { "lines",   OPT_REQ,  'n' },
        { "bytes",   OPT_REQ,  'c' },
        { NULL, 0, 0 }
    };
    opt_t o;
    int c;

    opt_init(&o, argc, argv, "qvn:c:", longs, 0);
    while ((c = opt_next(&o)) != OPT_DONE) {
        switch (c) {
        case 'q': opt_q = 1; break;
        case 'v': opt_v = 1; break;
        case 'n':
            if (parse_count(o.arg, &count, &negative) != 0) {
                err_msg("head", "invalid number of lines: '%s'", o.arg);
                return 1;
            }
            use_bytes = 0;
            break;
        case 'c':
            if (parse_count(o.arg, &count, &negative) != 0) {
                err_msg("head", "invalid number of bytes: '%s'", o.arg);
                return 1;
            }
            use_bytes = 1;
            break;
        case OPT_MISSING:
            err_usage("head", "[-n N] [-c N] [-qv] [FILE...]");
            return 1;
        default:
            /* Legacy: -NUM as shorthand for -n NUM */
            if (o.opt >= '0' && o.opt <= '9') {
                const char *p = opt_rest(&o);
                if (parse_count(p, &count, &negative) != 0) {
                    err_msg("head", "invalid number: '%s'", p);
                    return 1;
                }
                use_bytes = 0;
                break;
            }
            opt_err_unknown("head", &o);
            err_usage("head", "[-n N] [-c N] [-qv] [FILE...]");
            return 1;
        }
    }
    int i = o.ind;

    int nfiles = argc - i;

    if (nfiles == 0) {
        /* Read from stdin */
        if (opt_v)
            fprintf(stdout, "==> standard input <==\n");
        if (head_file("stdin", stdin, use_bytes, count, negative) != 0)
            ret = 1;
        return ret;
    }

    for (int j = i; j < argc; j++) {
        const char *fname = argv[j];

        /* Header: print unless -q; always print with -v; print for >1 file by default */
        int print_header = opt_v || (!opt_q && nfiles > 1);
        if (print_header) {
            /* Separate consecutive headers with blank line */
            if (j > i) fputc('\n', stdout);
            fprintf(stdout, "==> %s <==\n", fname);
        }

        FILE *fp;
        if (strcmp(fname, "-") == 0) {
            fp = stdin;
        } else {
            fp = fopen(fname, "r");
            if (!fp) {
                err_sys("head", "cannot open '%s'", fname);
                ret = 1;
                continue;
            }
            posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL);
        }

        if (head_file(fname, fp, use_bytes, count, negative) != 0)
            ret = 1;

        if (fp != stdin)
            fclose(fp);
    }

    return ret;
}
