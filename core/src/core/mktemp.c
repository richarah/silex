/* mktemp.c — mktemp builtin: create temporary files/directories */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int applet_mktemp(int argc, char **argv)
{
    int opt_d      = 0;   /* -d: create directory */
    int opt_u      = 0;   /* -u: dry-run (unsafe, just print) */
    const char *tmpdir   = NULL;  /* -p DIR */
    const char *tmpl     = NULL;  /* user template */
    int i;

    /* Respect TMPDIR env var */
    const char *envtmp = getenv("TMPDIR");
    if (!envtmp || envtmp[0] == '\0')
        envtmp = "/tmp";

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'd': opt_d = 1; break;
            case 'u': opt_u = 1; break;
            case 'p':
                if (p[1]) { tmpdir = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("mktemp", "[-d] [-p DIR] [TEMPLATE]");
                        return 1;
                    }
                    tmpdir = argv[i];
                    stop = 1;
                }
                break;
            case 't':
                /* -t: prefix only; deprecated but common */
                break;
            default:
                err_msg("mktemp", "unrecognized option '-%c'", *p);
                err_usage("mktemp", "[-d] [-p DIR] [TEMPLATE]");
                return 1;
            }
            p++;
        }
    }

    if (i < argc)
        tmpl = argv[i];

    /* Build the full template path */
    char path[4096];
    const char *base = tmpdir ? tmpdir : envtmp;

    if (tmpl) {
        /* User-supplied template: if it contains '/', use as-is, else prepend base */
        if (strchr(tmpl, '/')) {
            snprintf(path, sizeof(path), "%s", tmpl);
        } else {
            snprintf(path, sizeof(path), "%s/%s", base, tmpl);
        }
        /* Ensure at least 6 X's at the end */
        size_t len = strlen(path);
        size_t xs  = 0;
        while (xs < len && path[len - 1 - xs] == 'X') xs++;
        if (xs < 6) {
            if (len + (6 - xs) >= sizeof(path)) {
                err_msg("mktemp", "template too long");
                return 1;
            }
            for (size_t j = xs; j < 6; j++)
                path[len++] = 'X';
            path[len] = '\0';
        }
    } else {
        snprintf(path, sizeof(path), "%s/tmp.XXXXXXXXXX", base);
    }

    if (opt_u) {
        /* Dry-run: just print the template (unsafe, for compat only) */
        puts(path);
        return 0;
    }

    if (opt_d) {
        if (!mkdtemp(path)) {
            err_msg("mktemp", "failed to create directory: %s", path);
            return 1;
        }
    } else {
        int fd = mkstemp(path);
        if (fd < 0) {
            err_msg("mktemp", "failed to create file: %s", path);
            return 1;
        }
        close(fd);
    }

    puts(path);
    return 0;
}
