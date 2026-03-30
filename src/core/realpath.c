/* realpath.c — realpath builtin: resolve symlinks and canonicalize path */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/path.h"
#include "../util/error.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Compute a relative path from 'base_dir' to 'target' (both must be
 * absolute canonicalized paths).  Result written into 'out'. */
static void make_relative(const char *base_dir, const char *target,
                           char *out, size_t outsz)
{
    /* Count common prefix components */
    const char *b = base_dir;
    const char *t = target;

    /* Find longest common prefix at component boundary */
    const char *b_last_match = b;
    const char *t_last_match = t;

    while (*b && *t) {
        if (*b == *t) {
            if (*b == '/') {
                b_last_match = b;
                t_last_match = t;
            }
            b++; t++;
        } else {
            break;
        }
    }
    if ((*b == '\0' || *b == '/') && (*t == '\0' || *t == '/')) {
        b_last_match = b;
        t_last_match = t;
    }

    /* Consume the common prefix slash */
    b = b_last_match;
    t = t_last_match;
    if (*b == '/') b++;
    if (*t == '/') t++;

    /* Count remaining components in base (each needs a '..') */
    char result[PATH_MAX * 2];
    size_t pos = 0;
    result[0] = '\0';

    const char *p = b;
    while (*p) {
        if (*p == '/') {
            if (pos + 3 >= sizeof(result)) break;
            result[pos++] = '.';
            result[pos++] = '.';
            result[pos++] = '/';
        }
        p++;
    }
    if (*b != '\0') {
        /* There was at least one remaining component */
        if (pos + 3 >= sizeof(result)) {
            snprintf(out, outsz, "%s", target);
            return;
        }
        result[pos++] = '.';
        result[pos++] = '.';
        if (*t != '\0') result[pos++] = '/';
    }

    /* Append remaining target components */
    size_t tlen = strlen(t);
    if (pos + tlen + 1 <= sizeof(result)) {
        memcpy(result + pos, t, tlen);
        pos += tlen;
    }
    result[pos] = '\0';

    if (pos == 0) {
        result[0] = '.';
        result[1] = '\0';
    }

    snprintf(out, outsz, "%s", result);
}

int applet_realpath(int argc, char **argv)
{
    int opt_m           = 0;   /* -m: allow nonexistent components */
    const char *rel_to  = NULL; /* --relative-to=DIR */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strncmp(arg, "--relative-to=", 14) == 0) {
            rel_to = arg + 14;
            continue;
        }
        if (strcmp(arg, "--relative-to") == 0) {
            i++;
            if (i >= argc) {
                err_usage("realpath", "[-m] [--relative-to=DIR] PATH...");
                return 1;
            }
            rel_to = argv[i];
            continue;
        }

        if (arg[0] != '-' || arg[1] == '\0') break;

        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'm': opt_m = 1; break;
            case 'e': break; /* -e: all components must exist (default) */
            case 'L': break; /* -L: logical (resolve relative to cwd, no POSIX) */
            case 'P': break; /* -P: physical (default) */
            case 'q': break; /* -q: quiet */
            case 's': break; /* -s: no symlink resolution (GNU ext) */
            case 'z': break; /* -z: NUL separated output */
            default:
                err_msg("realpath", "unrecognized option '-%c'", *p);
                err_usage("realpath", "[-m] [--relative-to=DIR] PATH...");
                return 1;
            }
            p++;
        }
    }

    if (i >= argc) {
        err_usage("realpath", "[-m] [--relative-to=DIR] PATH...");
        return 1;
    }

    /* Resolve --relative-to base */
    char rel_base[PATH_MAX];
    if (rel_to) {
        if (!path_canon(rel_to, rel_base)) {
            /* Try making absolute relative to cwd */
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof(cwd))) {
                err_msg("realpath", "cannot get cwd");
                return 1;
            }
            if (rel_to[0] == '/') {
                strncpy(rel_base, rel_to, sizeof(rel_base) - 1);
                rel_base[sizeof(rel_base) - 1] = '\0';
            } else {
                int n = snprintf(rel_base, sizeof(rel_base), "%s/", cwd);
                if (n > 0 && (size_t)n < sizeof(rel_base))
                    strncat(rel_base + n, rel_to, sizeof(rel_base) - (size_t)n - 1);
            }
        }
    }

    int ret = 0;
    for (; i < argc; i++) {
        char resolved[PATH_MAX];

        if (opt_m) {
            /* Allow nonexistent: manually canonicalize without stat */
            /* Build absolute path first */
            char abs[PATH_MAX * 2];
            if (argv[i][0] == '/') {
                snprintf(abs, sizeof(abs), "%s", argv[i]);
            } else {
                char cwd[PATH_MAX];
                if (!getcwd(cwd, sizeof(cwd))) {
                    err_msg("realpath", "cannot get cwd");
                    ret = 1;
                    continue;
                }
                snprintf(abs, sizeof(abs), "%s/%s", cwd, argv[i]);
            }
            /* Normalize . and .. without resolving symlinks */
            char norm[PATH_MAX * 2];
            size_t npos = 0;
            norm[npos++] = '/';
            const char *p = abs;
            if (*p == '/') p++;
            while (*p) {
                const char *end = p;
                while (*end && *end != '/') end++;
                size_t complen = (size_t)(end - p);
                if (complen == 0 || (complen == 1 && p[0] == '.')) {
                    /* skip */
                } else if (complen == 2 && p[0] == '.' && p[1] == '.') {
                    /* go up */
                    if (npos > 1) {
                        npos--;
                        while (npos > 1 && norm[npos - 1] != '/') npos--;
                    }
                } else {
                    if (npos > 1) norm[npos++] = '/';
                    if (npos + complen < sizeof(norm)) {
                        memcpy(norm + npos, p, complen);
                        npos += complen;
                    }
                }
                p = end;
                if (*p == '/') p++;
            }
            norm[npos] = '\0';
            if (npos == 0) { norm[0] = '/'; norm[1] = '\0'; }
            strncpy(resolved, norm, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
        } else {
            /* Try full resolution first; on ENOENT, resolve dirname + basename
             * (GNU realpath default: resolve what exists, exit 0 if parent ok) */
            if (!path_canon(argv[i], resolved)) {
                char dir[PATH_MAX];
                path_dirname(argv[i], dir);
                char rdir[PATH_MAX];
                if (!path_canon(dir, rdir)) {
                    err_msg("realpath", "%s: %s", argv[i], strerror(errno));
                    ret = 1;
                    continue;
                }
                const char *b = path_basename(argv[i]);
                if (!b || b[0] == '\0' || strcmp(b, ".") == 0 || strcmp(b, "..") == 0) {
                    strncpy(resolved, rdir, sizeof(resolved) - 1);
                    resolved[sizeof(resolved) - 1] = '\0';
                } else {
                    int n = snprintf(resolved, sizeof(resolved), "%s/%s", rdir, b);
                    if (n < 0 || n >= (int)sizeof(resolved)) {
                        err_msg("realpath", "%s: %s", argv[i], "path too long");
                        ret = 1;
                        continue;
                    }
                }
            }
        }

        if (rel_to) {
            char out[PATH_MAX * 2];
            make_relative(rel_base, resolved, out, sizeof(out));
            puts(out);
        } else {
            puts(resolved);
        }
    }

    return ret;
}
