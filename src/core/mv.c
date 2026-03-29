/* mv.c — mv builtin: move (rename) files */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define MV_BUFSZ 65536

struct mv_opts {
    int force;       /* -f */
    int interactive; /* -i */
    int no_clobber;  /* -n */
    int verbose;     /* -v */
    int update;      /* -u */
    int no_target;   /* -T */
};

/* -------------------------------------------------------------------------
 * copy_reg_file: copy a regular file from src_fd to dst_path.
 * Preserves permissions from src_st.
 * Returns 0 on success, 1 on error.
 * ------------------------------------------------------------------------- */
static int copy_reg_file(int src_fd, const char *dst_path,
                         const struct stat *src_st)
{
    /* Open destination, preserving source permissions */
    mode_t mode = src_st->st_mode & 0777;
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dst_fd < 0) {
        err_sys("mv", "cannot create '%s'", dst_path);
        return 1;
    }

    char buf[MV_BUFSZ];
    ssize_t nread;
    int ret = 0;

    while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
        const char *p = buf;
        ssize_t rem = nread;
        while (rem > 0) {
            ssize_t nw = write(dst_fd, p, (size_t)rem);
            if (nw < 0) {
                err_sys("mv", "error writing '%s'", dst_path);
                ret = 1;
                goto done;
            }
            p   += nw;
            rem -= nw;
        }
    }
    if (nread < 0) {
        err_sys("mv", "error reading source");
        ret = 1;
    }

done:
    /* Restore permissions via fchmod after write */
    if (ret == 0) {
        if (fchmod(dst_fd, src_st->st_mode & 07777) != 0) {
            /* non-fatal: permissions are best-effort */
        }
    }
    close(dst_fd);
    return ret;
}

/* -------------------------------------------------------------------------
 * mv_cross_fs: copy src to dst then unlink src (cross-filesystem fallback).
 * Handles regular files only; for directories this is called recursively.
 * Returns 0 on success, 1 on error.
 * ------------------------------------------------------------------------- */
static int mv_cross_fs(const char *src, const char *dst,
                       const struct stat *src_st);

static int mv_cross_fs_dir(const char *src, const char *dst,
                           const struct stat *src_st);

static int mv_cross_fs(const char *src, const char *dst,
                       const struct stat *src_st)
{
    if (S_ISDIR(src_st->st_mode)) {
        return mv_cross_fs_dir(src, dst, src_st);
    }

    if (S_ISLNK(src_st->st_mode)) {
        char link_target[PATH_MAX];
        ssize_t n = readlink(src, link_target, sizeof(link_target) - 1);
        if (n < 0) {
            err_sys("mv", "cannot read symlink '%s'", src);
            return 1;
        }
        link_target[n] = '\0';
        unlink(dst); /* ignore errors */
        if (symlink(link_target, dst) != 0) {
            err_sys("mv", "cannot create symlink '%s'", dst);
            return 1;
        }
        if (unlink(src) != 0) {
            err_sys("mv", "cannot remove '%s'", src);
            return 1;
        }
        return 0;
    }

    if (!S_ISREG(src_st->st_mode)) {
        err_msg("mv", "cannot move special file '%s'", src);
        return 1;
    }

    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        err_sys("mv", "cannot open '%s'", src);
        return 1;
    }

    int ret = copy_reg_file(src_fd, dst, src_st);
    close(src_fd);

    if (ret == 0) {
        if (unlink(src) != 0) {
            err_sys("mv", "cannot remove source '%s'", src);
            ret = 1;
        }
    }
    return ret;
}

static int mv_cross_fs_dir(const char *src, const char *dst,
                           const struct stat *src_st)
{
    /* Create destination directory */
    struct stat dst_st;
    if (stat(dst, &dst_st) != 0) {
        if (errno != ENOENT) {
            err_sys("mv", "cannot stat '%s'", dst);
            return 1;
        }
        if (mkdir(dst, src_st->st_mode | 0700) != 0) {
            err_sys("mv", "cannot create directory '%s'", dst);
            return 1;
        }
    }

    DIR *dir = opendir(src);
    if (!dir) {
        err_sys("mv", "cannot open directory '%s'", src);
        return 1;
    }

    int ret = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char src_child[PATH_MAX], dst_child[PATH_MAX];
        if (!path_join(src, ent->d_name, src_child) ||
            !path_join(dst, ent->d_name, dst_child)) {
            err_msg("mv", "path too long");
            ret = 1;
            continue;
        }

        struct stat child_st;
        if (lstat(src_child, &child_st) != 0) {
            err_sys("mv", "cannot stat '%s'", src_child);
            ret = 1;
            continue;
        }

        if (mv_cross_fs(src_child, dst_child, &child_st) != 0)
            ret = 1;
    }
    closedir(dir);

    /* Apply permissions to dst directory */
    chmod(dst, src_st->st_mode & 07777); /* best effort */

    /* Remove source directory (must be empty now) */
    if (ret == 0) {
        if (rmdir(src) != 0) {
            err_sys("mv", "cannot remove source directory '%s'", src);
            ret = 1;
        }
    }
    return ret;
}

/* -------------------------------------------------------------------------
 * do_mv: move src to dst (atomic rename or cross-fs copy+unlink).
 * Returns 0 on success, 1 on error.
 * ------------------------------------------------------------------------- */
static int do_mv(const char *src, const char *dst, const struct mv_opts *opts)
{
    struct stat src_st;
    if (lstat(src, &src_st) != 0) {
        err_sys("mv", "cannot stat '%s'", src);
        return 1;
    }

    /* -u: skip if dst exists and is not older than src */
    if (opts->update) {
        struct stat dst_st;
        if (lstat(dst, &dst_st) == 0) {
            if (dst_st.st_mtim.tv_sec > src_st.st_mtim.tv_sec ||
                (dst_st.st_mtim.tv_sec == src_st.st_mtim.tv_sec &&
                 dst_st.st_mtim.tv_nsec >= src_st.st_mtim.tv_nsec)) {
                return 0; /* dst is same age or newer; skip */
            }
        }
    }

    /* Check if dst exists */
    struct stat dst_st;
    int dst_exists = (lstat(dst, &dst_st) == 0);

    if (dst_exists) {
        /* -n: no-clobber */
        if (opts->no_clobber) {
            return 0; /* skip silently */
        }

        /* -i: interactive prompt */
        if (opts->interactive && !opts->force) {
            fprintf(stderr, "mv: overwrite '%s'? ", dst);
            char resp[8];
            if (fgets(resp, sizeof(resp), stdin) == NULL || resp[0] != 'y')
                return 0;
        }

        /* If dst is a directory and src is not, refuse */
        if (S_ISDIR(dst_st.st_mode) && !S_ISDIR(src_st.st_mode)) {
            err_msg("mv", "cannot overwrite directory '%s' with non-directory", dst);
            return 1;
        }
        /* If dst is not a directory and src is, refuse */
        if (!S_ISDIR(dst_st.st_mode) && S_ISDIR(src_st.st_mode)) {
            err_msg("mv", "cannot move directory '%s' over non-directory '%s'",
                    src, dst);
            return 1;
        }
    }

    /* Try atomic rename first */
    if (rename(src, dst) == 0) {
        if (opts->verbose)
            printf("'%s' -> '%s'\n", src, dst);
        return 0;
    }

    if (errno != EXDEV) {
        err_sys("mv", "cannot move '%s' to '%s'", src, dst);
        return 1;
    }

    /* Cross-filesystem: copy then unlink */
    if (opts->verbose)
        printf("'%s' -> '%s'\n", src, dst);

    /* If dst exists and is a regular file/symlink, remove it first */
    if (dst_exists && !S_ISDIR(dst_st.st_mode)) {
        if (unlink(dst) != 0 && errno != ENOENT) {
            err_sys("mv", "cannot remove '%s'", dst);
            return 1;
        }
    }

    return mv_cross_fs(src, dst, &src_st);
}

/* -------------------------------------------------------------------------
 * applet_mv
 * ------------------------------------------------------------------------- */
int applet_mv(int argc, char **argv)
{
    struct mv_opts opts = {0};
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Long options */
        if (strcmp(arg, "--force") == 0)         { opts.force = 1; opts.interactive = 0; continue; }
        if (strcmp(arg, "--interactive") == 0)   { opts.interactive = 1; opts.force = 0; continue; }
        if (strcmp(arg, "--no-clobber") == 0)    { opts.no_clobber = 1; continue; }
        if (strcmp(arg, "--verbose") == 0)        { opts.verbose = 1; continue; }
        if (strcmp(arg, "--update") == 0)         { opts.update = 1; continue; }
        if (strcmp(arg, "--no-target-directory") == 0) { opts.no_target = 1; continue; }

        /* Short flags */
        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'f': opts.force = 1; opts.interactive = 0; break;
            case 'i': opts.interactive = 1; opts.force = 0; break;
            case 'n': opts.no_clobber = 1; break;
            case 'v': opts.verbose = 1; break;
            case 'u': opts.update = 1; break;
            case 'T': opts.no_target = 1; break;
            default:
                err_msg("mv", "unrecognized option '-%c'", *p);
                err_usage("mv", "[-finvuT] SOURCE... DEST");
                return 1;
            }
            p++;
        }
    }

    int noperands = argc - i;
    if (noperands < 2) {
        err_usage("mv", "[-finvuT] SOURCE... DEST");
        return 1;
    }

    const char *dst = argv[argc - 1];
    int nsrc = noperands - 1;

    /* Determine if dst is an existing directory */
    struct stat dst_st;
    int dst_exists  = (stat(dst, &dst_st) == 0);
    int dst_is_dir  = dst_exists && S_ISDIR(dst_st.st_mode);

    /* Multiple sources require dst to be a directory */
    if (nsrc > 1 && !dst_is_dir) {
        if (dst_exists)
            fprintf(stderr, "mv: target '%s' is not a directory\n", dst);
        else
            fprintf(stderr, "mv: target directory '%s': No such file or directory\n", dst);
        return 1;
    }

    /* -T: dst must not be a directory */
    if (opts.no_target && dst_is_dir) {
        fprintf(stderr, "mv: cannot overwrite directory '%s' with non-directory\n", dst);
        return 1;
    }

    for (int j = i; j < argc - 1; j++) {
        const char *src = argv[j];
        char real_dst[PATH_MAX];

        if (!opts.no_target && dst_is_dir) {
            const char *base = path_basename(src);
            if (!path_join(dst, base, real_dst)) {
                err_msg("mv", "path too long");
                ret = 1;
                continue;
            }
        } else {
            size_t dlen = strlen(dst);
            if (dlen >= PATH_MAX) {
                err_msg("mv", "path too long");
                ret = 1;
                continue;
            }
            memcpy(real_dst, dst, dlen + 1);
        }

        if (do_mv(src, real_dst, &opts) != 0)
            ret = 1;
    }

    return ret;
}
