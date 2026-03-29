/* mkdir.c — mkdir builtin: create directories */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Parse an octal or symbolic mode string.
 * For now only octal is supported (symbolic modes are rare in Dockerfiles).
 * Returns the mode, or (mode_t)-1 on error.
 */
static mode_t parse_mode_str(const char *s)
{
    if (*s >= '0' && *s <= '7') {
        char *end;
        unsigned long val = strtoul(s, &end, 8);
        if (*end != '\0' || val > 07777)
            return (mode_t)-1;
        return (mode_t)val;
    }
    /* TODO: symbolic mode parsing (chmod-style) — currently unsupported */
    return (mode_t)-1;
}

/*
 * Create directory path including all parents (-p behaviour).
 * verbose: if non-zero, print "mkdir: created directory 'PATH'" for each new dir.
 * mode: permissions for newly created directories.
 * Returns 0 on success, 1 on any error.
 */
static int mkdir_p(const char *path, mode_t mode, int verbose)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= PATH_MAX) {
        err_msg("mkdir", "path too long or empty");
        return 1;
    }

    memcpy(tmp, path, len + 1);

    /* Walk the path and create each component */
    for (char *p = tmp + (tmp[0] == '/' ? 1 : 0); ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';

            if (tmp[0] != '\0') {
                struct stat st;
                if (stat(tmp, &st) != 0) {
                    if (errno != ENOENT) {
                        err_sys("mkdir", "%s", tmp);
                        return 1;
                    }
                    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                        err_sys("mkdir", "%s", tmp);
                        return 1;
                    }
                    if (errno != EEXIST && verbose)
                        printf("mkdir: created directory '%s'\n", tmp);
                } else if (!S_ISDIR(st.st_mode)) {
                    err_msg("mkdir", "%s: Not a directory", tmp);
                    return 1;
                }
            }

            *p = saved;
            if (saved == '\0')
                break;
        }
    }

    return 0;
}

int applet_mkdir(int argc, char **argv)
{
    int opt_p = 0;
    int opt_v = 0;
    mode_t opt_m = 0777;
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Handle long options */
        if (strncmp(arg, "--mode=", 7) == 0) {
            mode_t m = parse_mode_str(arg + 7);
            if (m == (mode_t)-1) {
                err_msg("mkdir", "invalid mode: %s", arg + 7);
                return 1;
            }
            opt_m = m;
            continue;
        }
        if (strcmp(arg, "--parents") == 0) { opt_p = 1; continue; }
        if (strcmp(arg, "--verbose") == 0) { opt_v = 1; continue; }

        /* Short flags */
        const char *p = arg + 1;
        int unknown = 0;
        while (*p && !unknown) {
            switch (*p) {
            case 'p': opt_p = 1; break;
            case 'v': opt_v = 1; break;
            case 'm': {
                const char *mstr;
                if (p[1]) {
                    mstr = p + 1;
                    p += strlen(p) - 1; /* advance to end of this arg */
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("mkdir", "[-pvm MODE] DIR...");
                        return 1;
                    }
                    mstr = argv[i];
                }
                mode_t m = parse_mode_str(mstr);
                if (m == (mode_t)-1) {
                    err_msg("mkdir", "invalid mode: %s", mstr);
                    return 1;
                }
                opt_m = m;
                break;
            }
            default:
                err_msg("mkdir", "unrecognized option '-%c'", *p);
                err_usage("mkdir", "[-pvm MODE] DIR...");
                return 1;
            }
            p++;
        }
    }

    if (i >= argc) {
        err_usage("mkdir", "[-pvm MODE] DIR...");
        return 1;
    }

    for (; i < argc; i++) {
        const char *dir = argv[i];

        if (opt_p) {
            if (mkdir_p(dir, opt_m, opt_v) != 0)
                ret = 1;
        } else {
            if (mkdir(dir, opt_m) != 0) {
                err_sys("mkdir", "%s", dir);
                ret = 1;
            } else if (opt_v) {
                printf("mkdir: created directory '%s'\n", dir);
            }
        }
    }

    return ret;
}
