/* dirname.c — dirname builtin: strip last component from file name */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/opt.h"

#include <stdio.h>
#include <string.h>

/*
 * Compute the dirname of PATH and write it into OUT (must be at least
 * PATH_MAX bytes).  Follows POSIX dirname(3) semantics:
 *   - If path contains no '/', return "."
 *   - Strip trailing slashes, then strip last component, then strip
 *     trailing slashes from what remains.
 *   - If nothing remains, return "/".
 */
static void do_dirname(const char *path, char *out, size_t outsz)
{
    size_t len = strlen(path);

    /* Empty string -> "." */
    if (len == 0) {
        out[0] = '.';
        out[1] = '\0';
        return;
    }

    char tmp[4096];
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1;
    memcpy(tmp, path, len);
    tmp[len] = '\0';

    /* Strip trailing slashes (keep at least one character) */
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    /* Find last '/' */
    char *last_slash = strrchr(tmp, '/');

    if (!last_slash) {
        /* No slash at all: dirname is "." */
        if (outsz > 1) {
            out[0] = '.';
            out[1] = '\0';
        }
        return;
    }

    /* Truncate at last slash */
    *last_slash = '\0';
    len = (size_t)(last_slash - tmp);

    /* Strip trailing slashes from what remains */
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    /* If nothing left (was something like "/foo"), result is "/" */
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, tmp, len);
    out[len] = '\0';
}

int applet_dirname(int argc, char **argv)
{
    static const opt_long_t longs[] = {
        { "zero", OPT_BARE, 'z' },
        { NULL, 0, 0 }
    };
    int opt_z = 0; /* -z: NUL-terminated output */
    opt_t o;
    int c;

    opt_init(&o, argc, argv, "z", longs, 0);
    while ((c = opt_next(&o)) != OPT_DONE) {
        switch (c) {
        case 'z': opt_z = 1; break;
        default:
            opt_err_unknown("dirname", &o);
            err_usage("dirname", "[-z] NAME...");
            return 1;
        }
    }
    int i = o.ind;

    if (i >= argc) {
        err_usage("dirname", "[-z] NAME...");
        return 1;
    }

    char out[4096];
    char term = opt_z ? '\0' : '\n';

    for (; i < argc; i++) {
        do_dirname(argv[i], out, sizeof(out));
        fputs(out, stdout);
        putchar(term);
    }

    return 0;
}
