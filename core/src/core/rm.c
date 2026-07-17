/* rm.c — rm builtin: remove files and directories */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rm_opts {
    int recursive;   /* -r / -R */
    int force;       /* -f */
    int interactive; /* -i */
    int verbose;     /* -v */
};

/* -------------------------------------------------------------------------
 * is_root_path: return 1 if the path resolves to "/" after canonicalization.
 *
 * We also catch paths that are empty or consist only of slashes.
 * ------------------------------------------------------------------------- */
static int is_root_path(const char *path)
{
    /* Check for empty string */
    if (path == NULL || path[0] == '\0')
        return 1;

    /* Check if stripping trailing slashes leaves an empty string */
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    if (len == 1 && path[0] == '/')
        return 1;

    /* Try to canonicalize */
    char canon[PATH_MAX];
    if (path_canon(path, canon) != NULL) {
        if (strcmp(canon, "/") == 0)
            return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * prompt_user: ask yes/no question about path.
 * Returns 1 if user answered yes, 0 otherwise.
 * ------------------------------------------------------------------------- */
static int prompt_user(const char *question, const char *path)
{
    fprintf(stderr, "silex: rm: %s '%s'? ", question, path);
    fflush(stderr);
    char resp[8];
    if (fgets(resp, sizeof(resp), stdin) == NULL)
        return 0;
    return (resp[0] == 'y' || resp[0] == 'Y');
}

/* -------------------------------------------------------------------------
 * rm_recursive: recursively remove the contents of dir, then dir itself.
 * Returns 0 if all removals succeeded, 1 if any failed.
 * ------------------------------------------------------------------------- */
static int rm_recursive(const char *path, const struct rm_opts *opts);

static int rm_recursive(const char *path, const struct rm_opts *opts)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (opts->force && errno == ENOENT)
            return 0;
        err_sys("rm", "cannot stat '%s'", path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* Not a directory: remove directly */
        if (opts->interactive) {
            if (!prompt_user("remove", path))
                return 0;
        }
        if (unlink(path) != 0) {
            if (opts->force && errno == ENOENT)
                return 0;
            err_sys("rm", "cannot remove '%s'", path);
            return 1;
        }
        if (opts->verbose)
            printf("removed '%s'\n", path);
        return 0;
    }

    /* It's a directory: descend first */
    if (opts->interactive) {
        if (!prompt_user("descend into directory", path))
            return 0;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        err_sys("rm", "cannot open directory '%s'", path);
        return 1;
    }

    int ret = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        if (!path_join(path, ent->d_name, child)) {
            err_msg("rm", "path too long near '%s'", path);
            ret = 1;
            continue;
        }

        /* Safety: never recurse into "/" */
        if (is_root_path(child)) {
            err_msg("rm", "refusing to remove '/' (safety check on child path)");
            ret = 1;
            continue;
        }

        if (rm_recursive(child, opts) != 0)
            ret = 1;
    }
    closedir(dir);

    /* Now remove the directory itself */
    if (ret == 0) {
        if (opts->interactive) {
            if (!prompt_user("remove directory", path))
                return 0;
        }
        if (rmdir(path) != 0) {
            if (opts->force && errno == ENOENT)
                return 0;
            err_sys("rm", "cannot remove directory '%s'", path);
            ret = 1;
        } else {
            if (opts->verbose)
                printf("removed directory '%s'\n", path);
        }
    }

    return ret;
}

/* -------------------------------------------------------------------------
 * applet_rm
 * ------------------------------------------------------------------------- */
int applet_rm(int argc, char **argv)
{
    struct rm_opts opts = {0};
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        /* Long options */
        if (strcmp(arg, "--recursive") == 0) { opts.recursive = 1; continue; }
        if (strcmp(arg, "--force") == 0)      { opts.force = 1;     continue; }
        if (strcmp(arg, "--interactive") == 0){ opts.interactive = 1; continue; }
        if (strcmp(arg, "--verbose") == 0)    { opts.verbose = 1;   continue; }

        /* Explicitly reject --no-preserve-root for safety */
        if (strcmp(arg, "--no-preserve-root") == 0) {
            err_msg("rm", "--no-preserve-root is not supported for safety reasons");
            return 1;
        }
        /* Accept but ignore --preserve-root (it's the default) */
        if (strcmp(arg, "--preserve-root") == 0) {
            continue;
        }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Short flags */
        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'r': case 'R': opts.recursive = 1; break;
            case 'f': opts.force = 1; opts.interactive = 0; break;
            case 'i': opts.interactive = 1; opts.force = 0; break;
            case 'v': opts.verbose = 1; break;
            default:
                err_msg("rm", "unrecognized option '-%c'", *p);
                err_usage("rm", "[-rRfiv] FILE...");
                return 1;
            }
            p++;
        }
    }

    if (i >= argc) {
        if (opts.force)
            return 0; /* -f with no arguments is not an error */
        err_usage("rm", "[-rRfiv] FILE...");
        return 1;
    }

    for (; i < argc; i++) {
        const char *path = argv[i];

        /* Safety: refuse to operate on "/" */
        if (is_root_path(path)) {
            err_msg("rm", "it is dangerous to operate recursively on '/'");
            err_msg("rm", "use a different command if you are sure");
            ret = 1;
            continue;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            if (opts.force && errno == ENOENT)
                continue; /* -f silences missing-file errors */
            err_sys("rm", "cannot remove '%s'", path);
            ret = 1;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!opts.recursive) {
                err_msg("rm", "cannot remove '%s': Is a directory", path);
                ret = 1;
                continue;
            }

            if (rm_recursive(path, &opts) != 0)
                ret = 1;
        } else {
            /* Regular file, symlink, special file */
            if (opts.interactive) {
                if (!prompt_user("remove", path))
                    continue;
            }
            if (unlink(path) != 0) {
                if (opts.force && errno == ENOENT)
                    continue;
                err_sys("rm", "cannot remove '%s'", path);
                ret = 1;
            } else {
                if (opts.verbose)
                    printf("removed '%s'\n", path);
            }
        }
    }

    return ret;
}
