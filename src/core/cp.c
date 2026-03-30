/* cp.c — cp builtin: copy files and directories */

/* _GNU_SOURCE enables copy_file_range() declaration in <unistd.h> (glibc >= 2.27) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* Flags */
struct cp_opts {
    int recursive;    /* -r/-R */
    int preserve;     /* -p */
    int force;        /* -f */
    int interactive;  /* -i */
    int no_clobber;   /* -n */
    int update;       /* -u */
    int verbose;      /* -v */
    int no_target;    /* -T: treat destination as file */
    int follow_all;   /* -L: follow all symlinks */
    int follow_cmd;   /* -H: follow command-line symlinks */
    /* -P (default): don't follow symlinks in source tree */
};

/* Copy one regular file from src_fd to dst_path.
 * src_st: stat of source (for -p preserve).
 * Returns 0 on success, 1 on error. */
static int copy_file_fd(int src_fd, const char *dst_path,
                         const struct stat *src_st, const struct cp_opts *opts)
{
    int dst_fd;
    int flags = O_WRONLY | O_CREAT;

    if (opts->no_clobber)
        flags |= O_EXCL;
    else if (!opts->force)
        flags |= O_TRUNC;

    mode_t create_mode = opts->preserve ? src_st->st_mode : (src_st->st_mode & 0777);

    dst_fd = open(dst_path, flags, create_mode);
    if (dst_fd < 0) {
        if (errno == EEXIST && opts->no_clobber)
            return 0; /* silently skip */
        if (errno == EEXIST && opts->force) {
            /* Remove and retry */
            unlink(dst_path);
            dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, create_mode);
        }
        if (dst_fd < 0) {
            err_sys("cp", "cannot create regular file '%s'", dst_path);
            return 1;
        }
    }

    /* Copy data */
    int ret = 0;

#ifdef __linux__
    /* Try copy_file_range: kernel-to-kernel, zero user-space copy */
    {
        off_t file_size = src_st->st_size;
        if (file_size > 0) {
            off_t off_in = 0, off_out = 0;
            int cfr_ok = 1;
            while (off_in < file_size) {
                ssize_t n = copy_file_range(src_fd, &off_in, dst_fd, &off_out,
                                            (size_t)(file_size - off_in), 0);
                if (n < 0) {
                    if (errno == ENOSYS || errno == EXDEV || errno == EOPNOTSUPP
                        || errno == EINVAL) {
                        /* Kernel too old, cross-device, or unsupported fs */
                        cfr_ok = 0;
                        break;
                    }
                    err_sys("cp", "error copying '%s'", dst_path);
                    ret = 1;
                    goto done;
                }
                if (n == 0)
                    break;
            }
            if (cfr_ok)
                goto done;
            /* Fallback: seek back to start */
            if (lseek(src_fd, 0, SEEK_SET) < 0 ||
                lseek(dst_fd, 0, SEEK_SET) < 0 ||
                ftruncate(dst_fd, 0) < 0) {
                err_sys("cp", "error seeking for fallback copy of '%s'", dst_path);
                ret = 1;
                goto done;
            }
        } else if (file_size == 0) {
            goto done;  /* empty file: dst already created */
        }
    }
#endif /* __linux__ */

    /* Fallback: read/write loop */
    {
        char buf[65536];
        ssize_t nread;
        while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
            const char *p = buf;
            ssize_t remaining = nread;
            while (remaining > 0) {
                ssize_t nw = write(dst_fd, p, (size_t)remaining);
                if (nw < 0) {
                    err_sys("cp", "error writing '%s'", dst_path);
                    ret = 1;
                    goto done;
                }
                p += nw;
                remaining -= nw;
            }
        }
        if (nread < 0) {
            err_sys("cp", "error reading source");
            ret = 1;
        }
    }

done:
    /* Preserve attributes if -p */
    if (ret == 0 && opts->preserve) {
        /* Permissions already set at open(); set timestamps */
        struct timespec times[2];
        times[0] = src_st->st_atim;
        times[1] = src_st->st_mtim;
        futimens(dst_fd, times); /* best effort; ignore errors */
        /* Ownership: only succeeds if we have privilege */
        if (fchown(dst_fd, src_st->st_uid, src_st->st_gid) != 0) {
            /* non-fatal: continue without changing ownership */
        }
    }

    close(dst_fd);
    return ret;
}

/* Forward declaration for recursive use */
static int cp_path(const char *src, const char *dst,
                   int follow_src_symlink, const struct cp_opts *opts);

/* Recursively copy directory src into dst (dst must already exist or be created).
 * dst_is_new: if 1, dst was just created (preserve its attrs from src). */
static int copy_dir(const char *src, const char *dst, const struct cp_opts *opts)
{
    DIR *dir = opendir(src);
    if (!dir) {
        err_sys("cp", "cannot open directory '%s'", src);
        return 1;
    }

    /* Create destination directory if it doesn't exist */
    struct stat src_st;
    if (stat(src, &src_st) != 0) {
        err_sys("cp", "cannot stat '%s'", src);
        closedir(dir);
        return 1;
    }

    struct stat dst_st;
    if (stat(dst, &dst_st) != 0) {
        if (errno != ENOENT) {
            err_sys("cp", "cannot stat '%s'", dst);
            closedir(dir);
            return 1;
        }
        /* Create it */
        if (mkdir(dst, src_st.st_mode | 0700) != 0) {
            err_sys("cp", "cannot create directory '%s'", dst);
            closedir(dir);
            return 1;
        }
    }

    int ret = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char src_child[PATH_MAX], dst_child[PATH_MAX];
        if (!path_join(src, ent->d_name, src_child) ||
            !path_join(dst, ent->d_name, dst_child)) {
            err_msg("cp", "path too long");
            ret = 1;
            continue;
        }

        /* In recursive copy, -P means don't follow symlinks in source tree */
        int follow = opts->follow_all; /* -L follows all; -H/-P don't in tree */
        if (cp_path(src_child, dst_child, follow, opts) != 0)
            ret = 1;
    }

    closedir(dir);

    /* If -p, apply permissions/timestamps to dst after all children are copied */
    if (opts->preserve) {
        struct timespec times[2];
        times[0] = src_st.st_atim;
        times[1] = src_st.st_mtim;
        utimensat(AT_FDCWD, dst, times, 0); /* best effort */
        chmod(dst, src_st.st_mode);          /* best effort */
    }

    return ret;
}

/*
 * Copy src to dst.
 * follow_src_symlink: if 1, follow a symlink at the top-level src.
 * Returns 0 on success, 1 on error.
 */
static int cp_path(const char *src, const char *dst,
                   int follow_src_symlink, const struct cp_opts *opts)
{
    struct stat src_st;
    int stat_ret;

    if (follow_src_symlink)
        stat_ret = stat(src, &src_st);
    else
        stat_ret = lstat(src, &src_st);

    if (stat_ret != 0) {
        err_sys("cp", "cannot stat '%s'", src);
        return 1;
    }

    /* Handle symlinks in source: if not following, copy the link itself */
    if (S_ISLNK(src_st.st_mode)) {
        /* -P or -H (in recursive tree): copy symlink */
        char link_target[PATH_MAX];
        ssize_t n = readlink(src, link_target, sizeof(link_target) - 1);
        if (n < 0) {
            err_sys("cp", "cannot read symlink '%s'", src);
            return 1;
        }
        link_target[n] = '\0';

        /* Remove existing dst if needed */
        {
            struct stat _tmp;
            lstat(dst, &_tmp);
        }
        unlink(dst); /* ignore errors */

        if (symlink(link_target, dst) != 0) {
            err_sys("cp", "cannot create symlink '%s'", dst);
            return 1;
        }
        if (opts->verbose)
            printf("'%s' -> '%s'\n", src, dst);
        return 0;
    }

    /* Directory */
    if (S_ISDIR(src_st.st_mode)) {
        if (!opts->recursive) {
            fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
            return 1;
        }

        /* Check that we're not copying a directory into itself */
        struct stat dst_st;
        if (stat(dst, &dst_st) == 0) {
            if (dst_st.st_ino == src_st.st_ino &&
                dst_st.st_dev == src_st.st_dev) {
                err_msg("cp", "'%s' and '%s' are the same file", src, dst);
                return 1;
            }
        }

        if (opts->verbose)
            printf("'%s' -> '%s'\n", src, dst);
        return copy_dir(src, dst, opts);
    }

    /* Regular file */
    if (!S_ISREG(src_st.st_mode)) {
        /* Special files (device, socket, fifo): skip with warning */
        err_msg("cp", "cannot copy special file '%s'", src);
        return 1;
    }

    /* Check for update mode: skip if dest is newer than src */
    if (opts->update) {
        struct stat dst_st;
        if (stat(dst, &dst_st) == 0) {
            if (dst_st.st_mtim.tv_sec > src_st.st_mtim.tv_sec ||
                (dst_st.st_mtim.tv_sec == src_st.st_mtim.tv_sec &&
                 dst_st.st_mtim.tv_nsec >= src_st.st_mtim.tv_nsec))
                return 0;
        }
    }

    /* Interactive: ask if dest exists */
    if (opts->interactive) {
        struct stat dst_st;
        if (stat(dst, &dst_st) == 0) {
            fprintf(stderr, "cp: overwrite '%s'? ", dst);
            char resp[8];
            if (fgets(resp, sizeof(resp), stdin) == NULL || resp[0] != 'y')
                return 0;
        }
    }

    /* Open source */
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        err_sys("cp", "cannot open '%s'", src);
        return 1;
    }
    posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL); /* advisory; ignore errors */

    if (opts->verbose)
        printf("'%s' -> '%s'\n", src, dst);

    int ret = copy_file_fd(src_fd, dst, &src_st, opts);
    close(src_fd);
    return ret;
}

/*
 * Determine the actual destination path for a source file.
 * If dst is an existing directory (and -T not set), returns dst/basename(src).
 * Otherwise returns dst as-is.
 * Result written to result (PATH_MAX bytes).
 */
static char *resolve_dest(const char *src, const char *dst,
                           int dst_is_dir, int no_target,
                           char result[PATH_MAX])
{
    if (!no_target && dst_is_dir) {
        const char *base = path_basename(src);
        if (!path_join(dst, base, result))
            return NULL;
        return result;
    }
    size_t len = strlen(dst);
    if (len >= PATH_MAX)
        return NULL;
    memcpy(result, dst, len + 1);
    return result;
}

int applet_cp(int argc, char **argv)
{
    struct cp_opts opts = {0};
    /* Default: -P (don't follow symlinks in source) */
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        /* Long options */
        if (strcmp(arg, "--recursive") == 0)      { opts.recursive = 1; continue; }
        if (strcmp(arg, "--preserve") == 0)        { opts.preserve = 1; continue; }
        if (strcmp(arg, "--force") == 0)           { opts.force = 1; continue; }
        if (strcmp(arg, "--interactive") == 0)     { opts.interactive = 1; continue; }
        if (strcmp(arg, "--no-clobber") == 0)      { opts.no_clobber = 1; continue; }
        if (strcmp(arg, "--update") == 0)          { opts.update = 1; continue; }
        if (strcmp(arg, "--verbose") == 0)         { opts.verbose = 1; continue; }
        if (strcmp(arg, "--no-target-directory") == 0) { opts.no_target = 1; continue; }
        if (strcmp(arg, "--dereference") == 0)     { opts.follow_all = 1; continue; }
        if (strcmp(arg, "--no-dereference") == 0)  { opts.follow_all = 0; opts.follow_cmd = 0; continue; }
        if (strncmp(arg, "--preserve=", 11) == 0)  { opts.preserve = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Short flags */
        const char *p = arg + 1;
        int unknown_flag = 0;
        while (*p && !unknown_flag) {
            switch (*p) {
            case 'r': case 'R': opts.recursive = 1; break;
            case 'p': opts.preserve = 1; break;
            case 'f': opts.force = 1; opts.interactive = 0; break;
            case 'i': opts.interactive = 1; opts.force = 0; break;
            case 'n': opts.no_clobber = 1; break;
            case 'u': opts.update = 1; break;
            case 'v': opts.verbose = 1; break;
            case 'T': opts.no_target = 1; break;
            case 'L': opts.follow_all = 1; opts.follow_cmd = 0; break;
            case 'H': opts.follow_cmd = 1; opts.follow_all = 0; break;
            case 'P': opts.follow_all = 0; opts.follow_cmd = 0; break;
            case 'a':
                /* -a = -rpP */
                opts.recursive = 1;
                opts.preserve = 1;
                opts.follow_all = 0;
                opts.follow_cmd = 0;
                break;
            case 'd':
                /* -d = --no-dereference --preserve=links */
                opts.follow_all = 0;
                opts.follow_cmd = 0;
                break;
            default:
                /* Unknown flag: in Phase 1 we fall through to PATH (Phase 4 does module) */
                fprintf(stderr, "cp: unrecognized option '-%c'\n", *p);
                fprintf(stderr, "matchbox: cp: unsupported flag -%c. "
                        "Install matchbox-gnu-cp module or GNU coreutils.\n", *p);
                return 1;
            }
            p++;
        }
        if (unknown_flag)
            return 1;
    }

    /* Need at least src and dst */
    int noperands = argc - i;
    if (noperands < 2) {
        err_usage("cp", "[-rRpfivLHPantuT] SOURCE... DEST");
        return 1;
    }

    const char *dst = argv[argc - 1];
    int nsrc = noperands - 1;

    /* Determine if dst is an existing directory */
    struct stat dst_st;
    int dst_exists = (stat(dst, &dst_st) == 0);
    int dst_is_dir = dst_exists && S_ISDIR(dst_st.st_mode);

    /* Multiple sources: dst must be directory */
    if (nsrc > 1 && !dst_is_dir) {
        if (dst_exists)
            fprintf(stderr, "cp: target '%s' is not a directory\n", dst);
        else
            fprintf(stderr, "cp: target directory '%s': No such file or directory\n", dst);
        return 1;
    }

    /* -T: dst must NOT be an existing directory (or we'd put things inside) */
    if (opts.no_target && dst_is_dir) {
        fprintf(stderr, "cp: cannot overwrite directory '%s' with non-directory\n", dst);
        return 1;
    }

    for (int j = i; j < argc - 1; j++) {
        const char *src = argv[j];

        char real_dst[PATH_MAX];
        if (!resolve_dest(src, dst, dst_is_dir, opts.no_target, real_dst)) {
            err_msg("cp", "path too long");
            ret = 1;
            continue;
        }

        /* Follow symlink at command line for -H */
        int follow_top = opts.follow_all || opts.follow_cmd;

        if (cp_path(src, real_dst, follow_top, &opts) != 0)
            ret = 1;
    }

    return ret;
}
