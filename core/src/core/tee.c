/* tee.c — tee builtin: read stdin, write to stdout and files */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/opt.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEE_BUFSZ (65536)

int applet_tee(int argc, char **argv)
{
    int opt_a = 0;   /* -a: append mode */
    int opt_i = 0;   /* -i: ignore SIGINT */
    opt_t o;
    int c;

    opt_init(&o, argc, argv, "ai", NULL, 0);
    while ((c = opt_next(&o)) != OPT_DONE) {
        switch (c) {
        case 'a': opt_a = 1; break;
        case 'i': opt_i = 1; break;
        default:
            opt_err_unknown("tee", &o);
            err_usage("tee", "[-ai] [FILE]...");
            return 1;
        }
    }
    int i = o.ind;

    (void)opt_i;  /* signal handling not needed in subprocess context */

    int nfiles = argc - i;
    int *fds = NULL;
    if (nfiles > 0) {
        fds = malloc((size_t)nfiles * sizeof(int));
        if (!fds) {
            err_msg("tee", "out of memory");
            return 1;
        }
        int flags = O_WRONLY | O_CREAT | (opt_a ? O_APPEND : O_TRUNC);
        for (int k = 0; k < nfiles; k++) {
            fds[k] = open(argv[i + k], flags, 0666);
            if (fds[k] < 0) {
                err_msg("tee", "cannot open '%s'", argv[i + k]);
                /* keep going — POSIX says continue */
                fds[k] = -1;
            }
        }
    }

    char *buf = malloc(TEE_BUFSZ);
    if (!buf) {
        err_msg("tee", "out of memory");
        free(fds);
        return 1;
    }

    int ret = 0;
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, TEE_BUFSZ)) > 0) {
        /* Write to stdout */
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(STDOUT_FILENO, buf + written, (size_t)(n - written));
            if (w < 0) { ret = 1; break; }
            written += w;
        }
        /* Write to each file */
        for (int k = 0; k < nfiles; k++) {
            if (fds[k] < 0) { ret = 1; continue; }
            ssize_t fw = 0;
            while (fw < n) {
                ssize_t w = write(fds[k], buf + fw, (size_t)(n - fw));
                if (w < 0) { ret = 1; break; }
                fw += w;
            }
        }
    }
    if (n < 0)
        ret = 1;

    for (int k = 0; k < nfiles; k++)
        if (fds[k] >= 0) close(fds[k]);

    free(buf);
    free(fds);
    return ret;
}
