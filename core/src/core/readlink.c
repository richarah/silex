/* readlink.c — readlink builtin: print resolved symbolic link or canonical file name */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../util/error.h"
#include "../util/path.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Canonicalize a path where the last component may not exist (-f semantics):
 *   1. realpath() the dirname.
 *   2. Append the basename.
 * Result written to OUT (PATH_MAX bytes).
 * Returns 0 on success, -1 on error.
 */
static int canon_allow_missing_last(const char *path, char *out)
{
    char dir[PATH_MAX];
    char base[PATH_MAX];

    /* Get dirname into dir[] */
    path_dirname(path, dir);

    /* Resolve the dirname (must exist) */
    char resolved_dir[PATH_MAX];
    if (realpath(dir, resolved_dir) == NULL)
        return -1;

    /* Get basename */
    const char *b = path_basename(path);
    if (!b || b[0] == '\0') {
        /* path was "/" or similar — just use resolved_dir */
        size_t len = strlen(resolved_dir);
        if (len >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        memcpy(out, resolved_dir, len + 1);
        return 0;
    }

    /* If basename is "." or "..", resolve the full path too */
    if (strcmp(b, ".") == 0 || strcmp(b, "..") == 0) {
        /* Build full path and realpath it — it should exist */
        char full[PATH_MAX];
        if (!path_join(resolved_dir, b, full)) { errno = ENAMETOOLONG; return -1; }
        if (realpath(full, out) == NULL) return -1;
        return 0;
    }

    /* Join resolved_dir + basename */
    if (!path_join(resolved_dir, b, base)) { errno = ENAMETOOLONG; return -1; }
    size_t len = strlen(base);
    if (len >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, base, len + 1);
    return 0;
}

int applet_readlink(int argc, char **argv)
{
    int opt_f = 0; /* -f: canonicalize, last component may be missing */
    int opt_e = 0; /* -e: canonicalize, all components must exist */
    int opt_m = 0; /* -m: canonicalize, no existence checks at all */
    int opt_n = 0; /* -n: no newline */
    int opt_q = 0; /* -q: quiet (suppress errors) */
    int opt_z = 0; /* -z: NUL-terminated output */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strcmp(arg, "--canonicalize") == 0)               { opt_f = 1; continue; }
        if (strcmp(arg, "--canonicalize-existing") == 0)       { opt_e = 1; continue; }
        if (strcmp(arg, "--canonicalize-missing") == 0)        { opt_m = 1; continue; }
        if (strcmp(arg, "--no-newline") == 0)                  { opt_n = 1; continue; }
        if (strcmp(arg, "--quiet") == 0 ||
            strcmp(arg, "--silent") == 0)                      { opt_q = 1; continue; }
        if (strcmp(arg, "--zero") == 0)                        { opt_z = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'f': opt_f = 1; break;
            case 'e': opt_e = 1; break;
            case 'm': opt_m = 1; break;
            case 'n': opt_n = 1; break;
            case 'q': opt_q = 1; break;
            case 's': opt_q = 1; break; /* -s = --silent */
            case 'z': opt_z = 1; break;
            default:
                err_msg("readlink", "unrecognized option '-%c'", *p);
                err_usage("readlink", "[-femnqz] FILE...");
                return 1;
            }
            p++;
        }
        (void)stop;
    }

    if (i >= argc) {
        err_usage("readlink", "[-femnqz] FILE...");
        return 1;
    }

    int ret = 0;
    /* opt_n suppresses the terminator for single-arg non-z mode */
    int multi = (argc - i) > 1;

    for (int j = i; j < argc; j++) {
        const char *path = argv[j];
        char result[PATH_MAX];

        if (opt_e) {
            /* Full canonicalize; all components must exist */
            if (realpath(path, result) == NULL) {
                if (!opt_q)
                    err_sys("readlink", "%s", path);
                ret = 1;
                continue;
            }
        } else if (opt_f) {
            /* Canonicalize; last component may be missing.
             * Try realpath() first (handles symlinks in final component);
             * fall back to canon_allow_missing_last for dangling cases. */
            if (realpath(path, result) == NULL) {
                if (canon_allow_missing_last(path, result) != 0) {
                    if (!opt_q)
                        err_sys("readlink", "%s", path);
                    ret = 1;
                    continue;
                }
            }
        } else if (opt_m) {
            /* Canonicalize; no existence checks — use path_normalize */
            char abs[PATH_MAX];
            /* If relative, prepend cwd */
            if (path[0] != '/') {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof(cwd)) == NULL) {
                    if (!opt_q)
                        err_sys("readlink", "getcwd");
                    ret = 1;
                    continue;
                }
                if (!path_join(cwd, path, abs)) {
                    if (!opt_q)
                        err_msg("readlink", "path too long: %s", path);
                    ret = 1;
                    continue;
                }
            } else {
                size_t len = strlen(path);
                if (len >= PATH_MAX) {
                    if (!opt_q)
                        err_msg("readlink", "path too long: %s", path);
                    ret = 1;
                    continue;
                }
                memcpy(abs, path, len + 1);
            }
            if (!path_normalize(abs, result)) {
                if (!opt_q)
                    err_msg("readlink", "path too long: %s", path);
                ret = 1;
                continue;
            }
        } else {
            /* Plain readlink(2) */
            ssize_t n = readlink(path, result, sizeof(result) - 1);
            if (n < 0) {
                if (!opt_q)
                    err_sys("readlink", "%s", path);
                ret = 1;
                continue;
            }
            result[n] = '\0';
        }

        fputs(result, stdout);
        /* Suppress newline if -n and single arg and not -z */
        if (opt_z) {
            putchar('\0');
        } else if (!opt_n || multi) {
            putchar('\n');
        }
    }

    return ret;
}
