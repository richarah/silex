/* tail.c — tail builtin: output the last part of files */

/* _GNU_SOURCE for inotify_init1 etc. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------------- */

static int parse_count(const char *s, long long *val, int *plus_mode)
{
    *plus_mode = 0;
    if (*s == '+') {
        *plus_mode = 1;
        s++;
    } else if (*s == '-') {
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

/* ---- last-N-lines using circular pointer array ----------------------------- */

/*
 * Seek-based fast path for last-N-lines on seekable regular files.
 * Strategy: seek backward in blocks of TAIL_SEEK_CHUNK bytes from EOF,
 * scanning for newlines, until we have found n+1 newlines (or BOF).
 * Returns 0 on success, 1 on error, -1 if fp is not seekable (use fallback).
 */
#define TAIL_SEEK_CHUNK 65536

static int tail_lines_seek(FILE *fp, long long n)
{
    if (n <= 0) return 0; /* nothing to print */

    /* Get file size */
    if (fseeko(fp, 0, SEEK_END) != 0) return -1;
    off_t file_size = ftello(fp);
    if (file_size < 0) return -1;
    if (file_size == 0) return 0;

    char *buf = malloc(TAIL_SEEK_CHUNK);
    if (!buf) { err_msg("tail", "out of memory"); return 1; }

    long long newlines_found = 0;
    off_t scan_pos = file_size;
    off_t print_from = 0; /* default: from beginning */

    while (scan_pos > 0 && newlines_found <= n) {
        off_t block_start = scan_pos - TAIL_SEEK_CHUNK;
        if (block_start < 0) block_start = 0;
        size_t block_size = (size_t)(scan_pos - block_start);

        if (fseeko(fp, block_start, SEEK_SET) != 0) {
            free(buf);
            return -1;
        }
        size_t nr = fread(buf, 1, block_size, fp);
        if (nr == 0 || ferror(fp)) {
            free(buf);
            return -1;
        }

        /* Scan backward for newlines */
        for (ssize_t k = (ssize_t)nr - 1; k >= 0; k--) {
            if (buf[k] == '\n') {
                newlines_found++;
                if (newlines_found == n + 1) {
                    /* The byte just after this newline is where we print from */
                    print_from = block_start + (off_t)k + 1;
                    goto found_start;
                }
            }
        }
        scan_pos = block_start;
    }
    /* Reached beginning of file without finding n+1 newlines → print all */
    print_from = 0;

found_start:
    free(buf);

    /* Seek to print_from and copy to stdout */
    if (fseeko(fp, print_from, SEEK_SET) != 0) return -1;

    {
        char outbuf[65536];
        size_t nr;
        while ((nr = fread(outbuf, 1, sizeof(outbuf), fp)) > 0) {
            if (fwrite(outbuf, 1, nr, stdout) != nr) return 1;
        }
        if (ferror(fp)) return 1;
    }
    return 0;
}

/*
 * Read entire fp into a heap buffer, then find the last n newline-terminated
 * lines (or all lines if n == 0 means "all").  Writes to stdout.
 * plus_mode: skip first (n-1) lines instead of last n.
 */
static int tail_lines(FILE *fp, long long n, int plus_mode)
{
    /*
     * Fast path: for regular seekable files in last-N mode, seek backward
     * from EOF instead of reading the whole file.
     */
    if (!plus_mode && n > 0) {
        struct stat st;
        if (fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode)) {
            int r = tail_lines_seek(fp, n);
            if (r >= 0) return r;
            /* r == -1 means seek failed; fall through to full-read path */
            rewind(fp);
        }
    }

    /* Read entire file into memory */
    size_t used = 0, cap = 65536;
    char *data = malloc(cap);
    if (!data) { err_msg("tail", "out of memory"); return 1; }

    for (;;) {
        if (used == cap) {
            size_t newcap = cap * 2;
            if (newcap <= cap) {
                free(data);
                err_msg("tail", "file too large");
                return 1;
            }
            char *tmp = realloc(data, newcap);
            if (!tmp) { free(data); err_msg("tail", "out of memory"); return 1; }
            data = tmp;
            cap = newcap;
        }
        size_t nr = fread(data + used, 1, cap - used, fp);
        if (nr == 0) break;
        used += nr;
    }
    if (ferror(fp)) { free(data); return 1; }

    if (used == 0) { free(data); return 0; }

    if (plus_mode) {
        /* Skip first (n-1) lines, then print the rest */
        long long skip = (n > 0) ? n - 1 : 0;
        const char *p = data;
        const char *end = data + used;
        long long skipped = 0;
        while (skipped < skip && p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            p = nl + 1;
            skipped++;
        }
        size_t print_len = (size_t)(end - p);
        if (print_len > 0) {
            if (fwrite(p, 1, print_len, stdout) != print_len) {
                free(data);
                return 1;
            }
        }
        free(data);
        return 0;
    }

    /* Last-N mode: build array of line start offsets */
    /* Count newlines to determine how many lines total */
    long long total_lines = 0;
    for (size_t k = 0; k < used; k++) {
        if (data[k] == '\n') total_lines++;
    }
    /* If file doesn't end with newline, last partial line counts */
    if (used > 0 && data[used - 1] != '\n') total_lines++;

    long long print_from_line = (n >= total_lines) ? 0 : (total_lines - n);

    /* Walk lines again and print those from print_from_line onwards */
    const char *p = data;
    const char *end_p = data + used;
    long long lineno = 0;
    while (p < end_p) {
        const char *nl = memchr(p, '\n', (size_t)(end_p - p));
        const char *line_end = nl ? nl + 1 : end_p;
        if (lineno >= print_from_line) {
            size_t llen = (size_t)(line_end - p);
            if (fwrite(p, 1, llen, stdout) != llen) {
                free(data);
                return 1;
            }
        }
        lineno++;
        p = line_end;
    }

    free(data);
    return 0;
}

/* ---- last-N-bytes ---------------------------------------------------------- */

static int tail_bytes(FILE *fp, long long n, int plus_mode)
{
    /*
     * Fast path: for regular seekable files in last-N-bytes mode, seek
     * directly to (file_size - n) and copy from there.
     */
    if (!plus_mode && n > 0) {
        struct stat st;
        if (fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode)) {
            off_t file_size = st.st_size;
            off_t seek_pos = (n >= (long long)file_size) ? 0 : (file_size - (off_t)n);
            if (fseeko(fp, seek_pos, SEEK_SET) == 0) {
                char outbuf[65536];
                size_t nr;
                while ((nr = fread(outbuf, 1, sizeof(outbuf), fp)) > 0) {
                    if (fwrite(outbuf, 1, nr, stdout) != nr) return 1;
                }
                return ferror(fp) ? 1 : 0;
            }
            /* Seek failed — fall through */
            rewind(fp);
        }
    }

    size_t used = 0, cap = 65536;
    char *data = malloc(cap);
    if (!data) { err_msg("tail", "out of memory"); return 1; }

    for (;;) {
        if (used == cap) {
            size_t newcap = cap * 2;
            if (newcap <= cap) {
                free(data);
                err_msg("tail", "file too large");
                return 1;
            }
            char *tmp = realloc(data, newcap);
            if (!tmp) { free(data); err_msg("tail", "out of memory"); return 1; }
            data = tmp;
            cap = newcap;
        }
        size_t nr = fread(data + used, 1, cap - used, fp);
        if (nr == 0) break;
        used += nr;
    }
    if (ferror(fp)) { free(data); return 1; }

    const char *start;
    size_t print_len;

    if (plus_mode) {
        /* Skip first (n-1) bytes */
        long long skip = (n > 0) ? n - 1 : 0;
        if (skip >= (long long)used) {
            free(data);
            return 0;
        }
        start = data + skip;
        print_len = used - (size_t)skip;
    } else {
        /* Last n bytes */
        if (n >= (long long)used) {
            start = data;
            print_len = used;
        } else {
            start = data + used - (size_t)n;
            print_len = (size_t)n;
        }
    }

    if (print_len > 0) {
        if (fwrite(start, 1, print_len, stdout) != print_len) {
            free(data);
            return 1;
        }
    }
    free(data);
    return 0;
}

/* ---- follow mode via inotify ----------------------------------------------- */

/*
 * Try to check if process pid is still alive.
 * Returns 1 if alive, 0 if dead (ESRCH).
 */
static int pid_alive(pid_t pid)
{
    if (kill(pid, 0) == 0) return 1;
    if (errno == ESRCH) return 0;
    return 1; /* other error (EPERM etc.) — assume alive */
}

/*
 * Drain and print any new data from fp starting at *offset.
 * Updates *offset to the current file size.
 * Returns 0 on success, 1 on error.
 */
static int drain_new_data(const char *fname, long long *offset)
{
    FILE *fp = fopen(fname, "r");
    if (!fp) return 0; /* file may have been briefly unavailable */

    if (fseeko(fp, (off_t)*offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    char buf[4096];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (fwrite(buf, 1, nr, stdout) != nr) {
            fclose(fp);
            return 1;
        }
        *offset += (long long)nr;
    }
    fflush(stdout);
    fclose(fp);
    return 0;
}

static int follow_file(const char *fname, long long *offset, pid_t watch_pid)
{
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        /* Fall back to poll loop */
        for (;;) {
            if (watch_pid > 0 && !pid_alive(watch_pid)) return 0;
            if (drain_new_data(fname, offset) != 0) return 1;
            struct timespec ts = {0, 200000000L}; /* 200 ms */
            nanosleep(&ts, NULL);
        }
    }

    int wd = inotify_add_watch(ifd, fname,
                               IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF);
    if (wd < 0) {
        close(ifd);
        /* Fallback: poll */
        for (;;) {
            if (watch_pid > 0 && !pid_alive(watch_pid)) return 0;
            if (drain_new_data(fname, offset) != 0) return 1;
            struct timespec ts = {0, 200000000L};
            nanosleep(&ts, NULL);
        }
    }

    char evbuf[sizeof(struct inotify_event) + NAME_MAX + 1];
    struct pollfd pfd = { .fd = ifd, .events = POLLIN };

    for (;;) {
        if (watch_pid > 0 && !pid_alive(watch_pid)) break;

        int ready = poll(&pfd, 1, 200); /* 200 ms timeout */
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ready > 0 && (pfd.revents & POLLIN)) {
            ssize_t nbytes;
            while ((nbytes = read(ifd, evbuf, sizeof(evbuf))) > 0) {
                /* Process events — just drain new data after any event */
                (void)nbytes;
            }
            if (drain_new_data(fname, offset) != 0) break;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
    return 0;
}

/* ---- applet entry ---------------------------------------------------------- */

int applet_tail(int argc, char **argv)
{
    long long count  = 10;
    int plus_mode    = 0;
    int use_bytes    = 0;
    int opt_f        = 0;
    int opt_q        = 0;
    int opt_v        = 0;
    pid_t watch_pid  = 0;
    int ret          = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        if (strcmp(arg, "--follow") == 0)  { opt_f = 1; continue; }
        if (strcmp(arg, "--quiet") == 0 ||
            strcmp(arg, "--silent") == 0)  { opt_q = 1; continue; }
        if (strcmp(arg, "--verbose") == 0) { opt_v = 1; continue; }

        if (strncmp(arg, "--lines=", 8) == 0) {
            if (parse_count(arg + 8, &count, &plus_mode) != 0) {
                err_msg("tail", "invalid number of lines: '%s'", arg + 8);
                return 1;
            }
            use_bytes = 0;
            continue;
        }
        if (strncmp(arg, "--bytes=", 8) == 0) {
            if (parse_count(arg + 8, &count, &plus_mode) != 0) {
                err_msg("tail", "invalid number of bytes: '%s'", arg + 8);
                return 1;
            }
            use_bytes = 1;
            continue;
        }
        if (strncmp(arg, "--pid=", 6) == 0) {
            char *end;
            long pv = strtol(arg + 6, &end, 10);
            if (*end != '\0' || pv <= 0) {
                err_msg("tail", "invalid --pid value: '%s'", arg + 6);
                return 1;
            }
            watch_pid = (pid_t)pv;
            continue;
        }
        if (strcmp(arg, "--pid") == 0) {
            i++;
            if (i >= argc) {
                err_usage("tail", "[-n N] [-c N] [-fqv] [--pid PID] [FILE...]");
                return 1;
            }
            char *end;
            long pv = strtol(argv[i], &end, 10);
            if (*end != '\0' || pv <= 0) {
                err_msg("tail", "invalid --pid value: '%s'", argv[i]);
                return 1;
            }
            watch_pid = (pid_t)pv;
            continue;
        }

        /* Short flags */
        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'f': opt_f = 1; break;
            case 'q': opt_q = 1; break;
            case 'v': opt_v = 1; break;
            case 'n': {
                const char *val;
                if (p[1]) { val = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("tail", "[-n N] [-c N] [-fqv] [--pid PID] [FILE...]");
                        return 1;
                    }
                    val = argv[i];
                }
                if (parse_count(val, &count, &plus_mode) != 0) {
                    err_msg("tail", "invalid number of lines: '%s'", val);
                    return 1;
                }
                use_bytes = 0;
                stop = 1;
                break;
            }
            case 'c': {
                const char *val;
                if (p[1]) { val = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("tail", "[-n N] [-c N] [-fqv] [--pid PID] [FILE...]");
                        return 1;
                    }
                    val = argv[i];
                }
                if (parse_count(val, &count, &plus_mode) != 0) {
                    err_msg("tail", "invalid number of bytes: '%s'", val);
                    return 1;
                }
                use_bytes = 1;
                stop = 1;
                break;
            }
            default:
                err_msg("tail", "unrecognized option '-%c'", *p);
                err_usage("tail", "[-n N] [-c N] [-fqv] [--pid PID] [FILE...]");
                return 1;
            }
            p++;
        }
    }

    int nfiles = argc - i;

    if (nfiles == 0) {
        /* stdin */
        if (opt_v)
            fprintf(stdout, "==> standard input <==\n");
        if (use_bytes)
            ret = tail_bytes(stdin, count, plus_mode);
        else
            ret = tail_lines(stdin, count, plus_mode);
        return ret;
    }

    for (int j = i; j < argc; j++) {
        const char *fname = argv[j];

        int print_header = opt_v || (!opt_q && nfiles > 1);
        if (print_header) {
            if (j > i) fputc('\n', stdout);
            fprintf(stdout, "==> %s <==\n", fname);
        }

        FILE *fp;
        if (strcmp(fname, "-") == 0) {
            fp = stdin;
        } else {
            fp = fopen(fname, "r");
            if (!fp) {
                err_sys("tail", "cannot open '%s'", fname);
                ret = 1;
                continue;
            }
        }

        int rc;
        if (use_bytes)
            rc = tail_bytes(fp, count, plus_mode);
        else
            rc = tail_lines(fp, count, plus_mode);

        if (rc != 0) ret = 1;

        /* In follow mode we need the file offset after the initial read */
        long long offset = 0;
        if (opt_f && fp != stdin) {
            offset = (long long)ftello(fp);
            fclose(fp);
            fflush(stdout);
            if (follow_file(fname, &offset, watch_pid) != 0)
                ret = 1;
        } else {
            if (fp != stdin) fclose(fp);
        }
    }

    return ret;
}
