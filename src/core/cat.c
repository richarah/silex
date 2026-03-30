/* cat.c — cat builtin: concatenate and print files */

/* _GNU_SOURCE for splice() */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAT_BUFSZ 65536

struct cat_opts {
    int number_all;    /* -n: number all output lines */
    int number_nonblank; /* -b: number non-blank lines (overrides -n) */
    int squeeze;       /* -s: squeeze multiple adjacent blank lines */
    int show_nonprint; /* -v: show non-printing chars */
    int show_ends;     /* -E: show $ at end of each line */
    int show_tabs;     /* -T: show tabs as ^I */
};

/* -------------------------------------------------------------------------
 * print_visible
 *
 * Write one byte to stdout in -v encoding:
 *   - ordinary printable (0x20–0x7e) and \n, \t (when not -T): as-is
 *   - \t when -T: ^I
 *   - 0x00–0x1f (control, not \t not \n): ^X  where X = char + 0x40
 *   - 0x7f: ^?
 *   - 0x80–0x9f: M-^X  (high control)
 *   - 0xa0–0xff: M-x   (high printable)
 * Returns 0 on success, -1 on write error.
 * ------------------------------------------------------------------------- */
static int print_visible(unsigned char c, int show_tabs)
{
    if (c == '\t') {
        if (show_tabs) {
            if (putchar('^') == EOF || putchar('I') == EOF) return -1;
        } else {
            if (putchar('\t') == EOF) return -1;
        }
        return 0;
    }
    if (c == '\n') {
        /* newline is handled by the caller for -E; never reaches here */
        if (putchar('\n') == EOF) return -1;
        return 0;
    }
    if (c < 0x20) {
        /* control character */
        if (putchar('^') == EOF) return -1;
        if (putchar((char)(c + 0x40)) == EOF) return -1;
        return 0;
    }
    if (c == 0x7f) {
        if (putchar('^') == EOF || putchar('?') == EOF) return -1;
        return 0;
    }
    if (c >= 0x80) {
        if (putchar('M') == EOF || putchar('-') == EOF) return -1;
        unsigned char lo = c & 0x7f;
        if (lo < 0x20) {
            if (putchar('^') == EOF) return -1;
            if (putchar((char)(lo + 0x40)) == EOF) return -1;
        } else if (lo == 0x7f) {
            if (putchar('^') == EOF || putchar('?') == EOF) return -1;
        } else {
            if (putchar((char)lo) == EOF) return -1;
        }
        return 0;
    }
    /* ordinary printable */
    if (putchar((char)c) == EOF) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * cat_fd: process one open file descriptor according to opts.
 * Returns 0 on success, 1 on any I/O error.
 * ------------------------------------------------------------------------- */
static int cat_fd(int fd, const char *name, const struct cat_opts *opts)
{
    static char buf[CAT_BUFSZ];
    ssize_t nread;

    /* Line-mode processing (needed for -n, -b, -s, -E, -v, -T) */
    int line_mode = opts->number_all || opts->number_nonblank ||
                    opts->squeeze    || opts->show_nonprint   ||
                    opts->show_ends  || opts->show_tabs;

    if (!line_mode) {
#ifdef __linux__
        /*
         * splice() fast path: when the input is a regular file, use splice()
         * to move data via the kernel pipe (fd → internal pipe → stdout).
         * This avoids read+write round trips through user-space entirely.
         * Falls back to read/write on EINVAL / ENOSYS (e.g. non-splice-able fd).
         */
        {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
                /* Create a kernel pipe for splice staging */
                int pfd[2];
                if (pipe(pfd) == 0) {
                    off_t remaining = st.st_size;
                    int splice_ok = 1;
                    while (remaining > 0) {
                        /* Move up to 64KB from file into pipe */
                        size_t chunk = (remaining > 65536) ? 65536 : (size_t)remaining;
                        ssize_t moved = splice(fd, NULL, pfd[1], NULL, chunk, SPLICE_F_MOVE);
                        if (moved < 0) {
                            if (errno == EINVAL || errno == ENOSYS) {
                                splice_ok = 0;
                                break;
                            }
                            close(pfd[0]); close(pfd[1]);
                            err_sys("cat", "%s", name);
                            return 1;
                        }
                        if (moved == 0) break;
                        /* Move from pipe to stdout */
                        ssize_t written = 0;
                        while (written < moved) {
                            ssize_t n = splice(pfd[0], NULL, STDOUT_FILENO, NULL,
                                               (size_t)(moved - written), SPLICE_F_MOVE);
                            if (n < 0) {
                                if (errno == EINVAL || errno == ENOSYS) {
                                    /* stdout not spliceable (e.g. terminal) */
                                    splice_ok = 0;
                                    break;
                                }
                                close(pfd[0]); close(pfd[1]);
                                err_sys("cat", "write error");
                                return 1;
                            }
                            if (n == 0) break;
                            written += n;
                        }
                        if (!splice_ok) break;
                        remaining -= moved;
                    }
                    close(pfd[0]); close(pfd[1]);
                    if (splice_ok) return 0;
                    /* splice failed (terminal stdout): fall through to read/write */
                    /* Seek back to start for fallback */
                    lseek(fd, 0, SEEK_SET);
                }
            }
        }
#endif /* __linux__ */
        /* Fallback: read/write loop */
        while ((nread = read(fd, buf, sizeof(buf))) > 0) {
            const char *p = buf;
            ssize_t rem = nread;
            while (rem > 0) {
                ssize_t nw = write(STDOUT_FILENO, p, (size_t)rem);
                if (nw < 0) {
                    err_sys("cat", "write error");
                    return 1;
                }
                p += nw;
                rem -= nw;
            }
        }
        if (nread < 0) {
            err_sys("cat", "%s", name);
            return 1;
        }
        return 0;
    }

    /*
     * Line-mode: we reassemble lines from the raw buffer.
     * We maintain a small carry buffer for partial lines across read()s.
     * Strategy: scan byte-by-byte within each read chunk, collecting a line,
     * then output it with the appropriate decorations when we hit '\n'.
     */
    static char line[65536]; /* maximum line accumulator */
    size_t line_len = 0;

    long lineno      = 0; /* counter for -n (all lines) */
    long lineno_nb   = 0; /* counter for -b (non-blank lines) */
    int  prev_blank  = 0; /* for -s squeeze */
    int  at_bol      = 1; /* at beginning of a line? */

    (void)at_bol; /* suppress unused warning when not used below */

    int ret = 0;

    while ((nread = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t idx = 0; idx < nread; idx++) {
            unsigned char c = (unsigned char)buf[idx];

            if (c == '\n') {
                /* End of line: flush line buffer with decorations */
                int is_blank = (line_len == 0);

                /* -s: squeeze consecutive blank lines */
                if (opts->squeeze && is_blank && prev_blank) {
                    /* skip this blank line entirely */
                    continue;
                }
                prev_blank = is_blank;

                /* Determine line number to print */
                if (opts->number_nonblank) {
                    /* -b: only number non-blank lines */
                    if (!is_blank) {
                        lineno_nb++;
                        if (printf("%6ld\t", lineno_nb) < 0) {
                            err_sys("cat", "write error");
                            ret = 1;
                            goto done;
                        }
                    }
                } else if (opts->number_all) {
                    lineno++;
                    if (printf("%6ld\t", lineno) < 0) {
                        err_sys("cat", "write error");
                        ret = 1;
                        goto done;
                    }
                }

                /* Print line content */
                if (opts->show_nonprint) {
                    for (size_t k = 0; k < line_len; k++) {
                        unsigned char lc = (unsigned char)line[k];
                        if (lc == '\t') {
                            if (opts->show_tabs) {
                                if (putchar('^') == EOF || putchar('I') == EOF) {
                                    err_sys("cat", "write error");
                                    ret = 1; goto done;
                                }
                            } else {
                                if (putchar('\t') == EOF) {
                                    err_sys("cat", "write error");
                                    ret = 1; goto done;
                                }
                            }
                        } else {
                            if (print_visible(lc, opts->show_tabs) < 0) {
                                err_sys("cat", "write error");
                                ret = 1; goto done;
                            }
                        }
                    }
                } else if (opts->show_tabs) {
                    for (size_t k = 0; k < line_len; k++) {
                        unsigned char lc = (unsigned char)line[k];
                        if (lc == '\t') {
                            if (putchar('^') == EOF || putchar('I') == EOF) {
                                err_sys("cat", "write error");
                                ret = 1; goto done;
                            }
                        } else {
                            if (putchar((char)lc) == EOF) {
                                err_sys("cat", "write error");
                                ret = 1; goto done;
                            }
                        }
                    }
                } else {
                    /* Plain output of accumulated line */
                    if (line_len > 0) {
                        const char *lp = line;
                        size_t rem2 = line_len;
                        while (rem2 > 0) {
                            ssize_t nw = write(STDOUT_FILENO, lp, rem2);
                            if (nw < 0) {
                                err_sys("cat", "write error");
                                ret = 1; goto done;
                            }
                            lp  += nw;
                            rem2 -= (size_t)nw;
                        }
                    }
                }

                /* -E: print $ before newline */
                if (opts->show_ends) {
                    if (putchar('$') == EOF) {
                        err_sys("cat", "write error");
                        ret = 1; goto done;
                    }
                }

                if (putchar('\n') == EOF) {
                    err_sys("cat", "write error");
                    ret = 1; goto done;
                }

                line_len = 0;
            } else {
                /* Accumulate into line buffer */
                if (line_len < sizeof(line) - 1) {
                    line[line_len++] = (char)c;
                } else {
                    /* Line too long: flush what we have and continue */
                    const char *lp = line;
                    size_t rem2 = line_len;
                    while (rem2 > 0) {
                        ssize_t nw = write(STDOUT_FILENO, lp, rem2);
                        if (nw < 0) {
                            err_sys("cat", "write error");
                            ret = 1; goto done;
                        }
                        lp  += nw;
                        rem2 -= (size_t)nw;
                    }
                    line_len = 0;
                    line[line_len++] = (char)c;
                }
            }
        }
    }

    if (nread < 0) {
        err_sys("cat", "%s", name);
        ret = 1;
    }

    /* Flush any remaining partial line (file without trailing newline) */
    if (line_len > 0) {
        if (opts->show_nonprint || opts->show_tabs) {
            for (size_t k = 0; k < line_len; k++) {
                unsigned char lc = (unsigned char)line[k];
                if (opts->show_nonprint) {
                    if (print_visible(lc, opts->show_tabs) < 0) {
                        err_sys("cat", "write error");
                        ret = 1; goto done;
                    }
                } else {
                    if (lc == '\t') {
                        if (putchar('^') == EOF || putchar('I') == EOF) {
                            err_sys("cat", "write error");
                            ret = 1; goto done;
                        }
                    } else {
                        if (putchar((char)lc) == EOF) {
                            err_sys("cat", "write error");
                            ret = 1; goto done;
                        }
                    }
                }
            }
        } else {
            const char *lp = line;
            size_t rem2 = line_len;
            while (rem2 > 0) {
                ssize_t nw = write(STDOUT_FILENO, lp, rem2);
                if (nw < 0) {
                    err_sys("cat", "write error");
                    ret = 1; goto done;
                }
                lp  += nw;
                rem2 -= (size_t)nw;
            }
        }
    }

done:
    return ret;
}

/* -------------------------------------------------------------------------
 * applet_cat
 * ------------------------------------------------------------------------- */
int applet_cat(int argc, char **argv)
{
    struct cat_opts opts = {0};
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int done_flags = 0;
        while (*p && !done_flags) {
            switch (*p) {
            case 'u':
                /* -u: unbuffered; no-op (we use read/write directly) */
                break;
            case 'n':
                opts.number_all = 1;
                break;
            case 'b':
                opts.number_nonblank = 1;
                opts.number_all = 0; /* -b overrides -n */
                break;
            case 's':
                opts.squeeze = 1;
                break;
            case 'v':
                opts.show_nonprint = 1;
                break;
            case 'e':
                /* -e implies -v and -E */
                opts.show_nonprint = 1;
                opts.show_ends = 1;
                break;
            case 't':
                /* -t implies -v and -T */
                opts.show_nonprint = 1;
                opts.show_tabs = 1;
                break;
            case 'A':
                /* -A implies -v, -E, -T */
                opts.show_nonprint = 1;
                opts.show_ends = 1;
                opts.show_tabs = 1;
                break;
            case 'E':
                opts.show_ends = 1;
                break;
            case 'T':
                opts.show_tabs = 1;
                break;
            default:
                err_msg("cat", "unrecognized option '-%c'", *p);
                err_usage("cat", "[-unbs veTtA] [FILE...]");
                return 1;
            }
            p++;
        }
        if (done_flags)
            break;
    }

    /* -b overrides -n per POSIX */
    if (opts.number_nonblank)
        opts.number_all = 0;

    if (i >= argc) {
        /* No file arguments: read stdin */
        return cat_fd(STDIN_FILENO, "(standard input)", &opts);
    }

    for (; i < argc; i++) {
        const char *path = argv[i];
        int fd;
        int close_fd = 1;

        if (strcmp(path, "-") == 0) {
            fd = STDIN_FILENO;
            close_fd = 0;
        } else {
            fd = open(path, O_RDONLY);
            if (fd < 0) {
                err_sys("cat", "%s", path);
                ret = 1;
                continue;
            }
            posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL); /* advisory */
        }

        if (cat_fd(fd, path, &opts) != 0)
            ret = 1;

        if (close_fd)
            close(fd);
    }

    return ret;
}
