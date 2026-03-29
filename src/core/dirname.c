#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* dirname.c — dirname builtin */

#include "../util/error.h"

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
    int opt_z = 0; /* -z: NUL-terminated output */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }
        if (strcmp(arg, "--zero") == 0) { opt_z = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'z': opt_z = 1; break;
            default:
                err_msg("dirname", "unrecognized option '-%c'", *p);
                err_usage("dirname", "[-z] NAME...");
                return 1;
            }
            p++;
        }
    }

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
