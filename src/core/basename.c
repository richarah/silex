/* basename.c -- basename builtin: strip directory and suffix from filenames */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* basename.c — basename builtin */

#include "../util/error.h"
#include "../util/path.h"

#include <stdio.h>
#include <string.h>

/*
 * Compute basename of PATH, storing result in OUT (must be at least
 * PATH_MAX bytes).  Trailing slashes are stripped, then the component
 * after the last '/' is taken.  If SUFFIX is non-NULL it is removed from
 * the end of the result.
 */
static void do_basename(const char *path, const char *suffix, char *out)
{
    /* Copy path so we can modify it */
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0) {
        out[0] = '.';
        out[1] = '\0';
        return;
    }
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1;
    memcpy(tmp, path, len);
    tmp[len] = '\0';

    /* Strip trailing slashes unless the whole string is slashes */
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    /* Find last '/' */
    const char *base = strrchr(tmp, '/');
    if (base)
        base++;
    else
        base = tmp;

    size_t blen = strlen(base);

    /* Remove suffix if given and it matches the end */
    if (suffix && suffix[0] != '\0') {
        size_t slen = strlen(suffix);
        if (blen > slen && strcmp(base + blen - slen, suffix) == 0)
            blen -= slen;
    }

    memcpy(out, base, blen);
    out[blen] = '\0';
}

int applet_basename(int argc, char **argv)
{
    int opt_a  = 0;   /* -a: treat every operand as NAME */
    int opt_z  = 0;   /* -z: NUL-terminated output */
    const char *opt_s = NULL; /* -s SUFFIX */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strncmp(arg, "--suffix=", 9) == 0) {
            opt_s = arg + 9;
            opt_a = 1; /* -s implies -a */
            continue;
        }
        if (strcmp(arg, "--multiple") == 0)  { opt_a = 1; continue; }
        if (strcmp(arg, "--zero") == 0)       { opt_z = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'a': opt_a = 1; break;
            case 'z': opt_z = 1; break;
            case 's':
                opt_a = 1; /* -s implies -a */
                if (p[1]) {
                    opt_s = p + 1;
                    stop = 1; /* rest of arg is suffix */
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("basename", "[-az] [-s SUFFIX] NAME...");
                        return 1;
                    }
                    opt_s = argv[i];
                }
                break;
            default:
                err_msg("basename", "unrecognized option '-%c'", *p);
                err_usage("basename", "[-az] [-s SUFFIX] NAME...");
                return 1;
            }
            p++;
        }
    }

    int nargs = argc - i;

    /* Classic two-argument form: basename NAME SUFFIX (no flags) */
    if (!opt_a && opt_s == NULL && nargs == 2) {
        opt_s = argv[i + 1];
        nargs = 1;
    }

    if (nargs < 1) {
        err_usage("basename", "[-az] [-s SUFFIX] NAME...");
        return 1;
    }

    /* Without -a, only one NAME is allowed */
    if (!opt_a && nargs > 1) {
        err_msg("basename", "extra operand '%s'", argv[i + 1]);
        err_usage("basename", "[-az] [-s SUFFIX] NAME...");
        return 1;
    }

    char out[4096];
    char term = opt_z ? '\0' : '\n';

    for (int j = 0; j < nargs; j++) {
        do_basename(argv[i + j], opt_s, out);
        fputs(out, stdout);
        putchar(term);
    }

    return 0;
}
