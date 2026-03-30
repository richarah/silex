/* install.c -- install builtin: copy files and set attributes */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* install.c — install builtin */

#include "../util/error.h"
#include "../util/path.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Parse an octal or symbolic mode string.
 * Currently supports octal only.  Returns (mode_t)-1 on failure.
 */
static mode_t parse_mode(const char *s)
{
    if (*s >= '0' && *s <= '7') {
        char *end;
        unsigned long val = strtoul(s, &end, 8);
        if (*end != '\0' || val > 07777)
            return (mode_t)-1;
        return (mode_t)val;
    }
    return (mode_t)-1;
}

/*
 * Look up UID for name (numeric string or username).
 * Returns 0 on success, -1 if not found.
 */
static int resolve_uid(const char *name, uid_t *out)
{
    /* Try numeric first */
    char *endp;
    unsigned long val = strtoul(name, &endp, 10);
    if (*endp == '\0') {
        *out = (uid_t)val;
        return 0;
    }
    struct passwd *pw = getpwnam(name);
    if (!pw) return -1;
    *out = pw->pw_uid;
    return 0;
}

/*
 * Look up GID for name (numeric string or group name).
 * Returns 0 on success, -1 if not found.
 */
static int resolve_gid(const char *name, gid_t *out)
{
    char *endp;
    unsigned long val = strtoul(name, &endp, 10);
    if (*endp == '\0') {
        *out = (gid_t)val;
        return 0;
    }
    struct group *gr = getgrnam(name);
    if (!gr) return -1;
    *out = gr->gr_gid;
    return 0;
}

/*
 * Copy file src_path to dst_path preserving (optionally) timestamps.
 * Returns 0 on success, 1 on error.
 */
static int copy_file(const char *src_path, const char *dst_path,
                     mode_t mode, int preserve_ts)
{
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        err_sys("install", "cannot open '%s'", src_path);
        return 1;
    }

    struct stat src_st;
    if (fstat(src_fd, &src_st) != 0) {
        err_sys("install", "cannot stat '%s'", src_path);
        close(src_fd);
        return 1;
    }

    /* Remove dest if it exists (install always overwrites) */
    unlink(dst_path); /* ignore errors */

    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (dst_fd < 0) {
        err_sys("install", "cannot create '%s'", dst_path);
        close(src_fd);
        return 1;
    }

    char buf[65536];
    ssize_t nread;
    int ret = 0;

    while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
        const char *p = buf;
        ssize_t rem = nread;
        while (rem > 0) {
            ssize_t nw = write(dst_fd, p, (size_t)rem);
            if (nw < 0) {
                err_sys("install", "write error on '%s'", dst_path);
                ret = 1;
                goto done;
            }
            p += nw;
            rem -= nw;
        }
    }
    if (nread < 0) {
        err_sys("install", "read error on '%s'", src_path);
        ret = 1;
    }

done:
    if (ret == 0 && preserve_ts) {
        struct timespec times[2];
        times[0] = src_st.st_atim;
        times[1] = src_st.st_mtim;
        futimens(dst_fd, times); /* best effort */
    }

    close(src_fd);
    close(dst_fd);
    return ret;
}

/*
 * Run strip(1) on path using fork+exec.
 * Returns 0 on success, 1 on failure.
 */
static int run_strip(const char *path)
{
    pid_t pid = fork();
    if (pid < 0) {
        err_sys("install", "fork failed");
        return 1;
    }
    if (pid == 0) {
        /* child */
        execl("/usr/bin/strip", "strip", path, (char *)NULL);
        /* fallback */
        execlp("strip", "strip", path, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        err_sys("install", "waitpid failed");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err_msg("install", "strip failed on '%s'", path);
        return 1;
    }
    return 0;
}

/*
 * Make a backup of dst_path by renaming it to dst_path + suffix.
 * Returns 0 on success, 1 on failure (or if dst doesn't exist).
 */
static int make_backup(const char *dst_path, const char *suffix)
{
    /* Check if dst exists */
    struct stat st;
    if (stat(dst_path, &st) != 0)
        return 0; /* no backup needed */

    char backup[PATH_MAX];
    int n = snprintf(backup, sizeof(backup), "%s%s", dst_path, suffix);
    if (n < 0 || (size_t)n >= sizeof(backup)) {
        err_msg("install", "backup path too long");
        return 1;
    }

    if (rename(dst_path, backup) != 0) {
        err_sys("install", "cannot create backup '%s'", backup);
        return 1;
    }
    return 0;
}

/*
 * Create a directory and all parents (mkdir -p semantics).
 * Returns 0 on success, 1 on error.
 */
static int mkdirs(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX) {
        err_msg("install", "path too long or empty");
        return 1;
    }
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + (tmp[0] == '/' ? 1 : 0); ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (tmp[0] != '\0') {
                if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                    err_sys("install", "cannot create directory '%s'", tmp);
                    return 1;
                }
            }
            *p = saved;
            if (saved == '\0') break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */

int applet_install(int argc, char **argv)
{
    int opt_d       = 0;
    int opt_v       = 0;
    int opt_p       = 0;   /* -p: preserve timestamps */
    int opt_s       = 0;   /* -s: strip */
    mode_t opt_m    = 0755;
    int   have_mode = 0;
    const char *opt_owner  = NULL;
    const char *opt_group  = NULL;
    const char *opt_suffix = NULL; /* -S SUFFIX: backup suffix */
    const char *opt_target = NULL; /* -t DIR: target directory */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strncmp(arg, "--mode=", 7) == 0) {
            mode_t m = parse_mode(arg + 7);
            if (m == (mode_t)-1) {
                err_msg("install", "invalid mode '%s'", arg + 7);
                return 1;
            }
            opt_m = m; have_mode = 1;
            continue;
        }
        if (strncmp(arg, "--owner=", 8) == 0) { opt_owner = arg + 8; continue; }
        if (strncmp(arg, "--group=", 8) == 0) { opt_group = arg + 8; continue; }
        if (strncmp(arg, "--suffix=", 9) == 0) { opt_suffix = arg + 9; continue; }
        if (strncmp(arg, "--target-directory=", 19) == 0) {
            opt_target = arg + 19; continue;
        }
        if (strcmp(arg, "--directory") == 0)  { opt_d = 1; continue; }
        if (strcmp(arg, "--verbose") == 0)     { opt_v = 1; continue; }
        if (strcmp(arg, "--preserve-timestamps") == 0) { opt_p = 1; continue; }
        if (strcmp(arg, "--strip") == 0)       { opt_s = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'd': opt_d = 1; break;
            case 'v': opt_v = 1; break;
            case 'p': opt_p = 1; break;
            case 's': opt_s = 1; break;
            case 'm': {
                const char *mstr;
                if (p[1]) {
                    mstr = p + 1;
                    stop = 1;
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("install", "[-dvps] [-m MODE] [-o OWNER] [-g GROUP] SRC... DST");
                        return 1;
                    }
                    mstr = argv[i];
                }
                mode_t m = parse_mode(mstr);
                if (m == (mode_t)-1) {
                    err_msg("install", "invalid mode '%s'", mstr);
                    return 1;
                }
                opt_m = m; have_mode = 1;
                break;
            }
            case 'o': {
                if (p[1]) { opt_owner = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("install", "[-dvps] [-m MODE] [-o OWNER] [-g GROUP] SRC... DST");
                        return 1;
                    }
                    opt_owner = argv[i];
                }
                break;
            }
            case 'g': {
                if (p[1]) { opt_group = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("install", "[-dvps] [-m MODE] [-o OWNER] [-g GROUP] SRC... DST");
                        return 1;
                    }
                    opt_group = argv[i];
                }
                break;
            }
            case 'S': {
                if (p[1]) { opt_suffix = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("install", "[-dvps] [-S SUFFIX] SRC... DST");
                        return 1;
                    }
                    opt_suffix = argv[i];
                }
                break;
            }
            case 't': {
                if (p[1]) { opt_target = p + 1; stop = 1; }
                else {
                    i++;
                    if (i >= argc) {
                        err_usage("install", "[-dvps] [-t DIR] SRC...");
                        return 1;
                    }
                    opt_target = argv[i];
                }
                break;
            }
            default:
                err_msg("install", "unrecognized option '-%c'", *p);
                err_usage("install", "[-dvps] [-m MODE] [-o OWNER] [-g GROUP] SRC... DST");
                return 1;
            }
            p++;
        }
    }

    /* If -d: create directories */
    if (opt_d) {
        if (i >= argc) {
            err_usage("install", "-d [-m MODE] [-v] DIR...");
            return 1;
        }
        mode_t dir_mode = have_mode ? opt_m : 0755;
        int ret = 0;
        for (; i < argc; i++) {
            if (opt_v)
                printf("install: creating directory '%s'\n", argv[i]);
            if (mkdirs(argv[i], dir_mode) != 0)
                ret = 1;
            else {
                /* Apply exact mode (mkdir uses umask) */
                chmod(argv[i], dir_mode); /* best effort */
            }
        }
        return ret;
    }

    /* Normal install: SRC... DST  or  -t DIR SRC... */
    int ret = 0;

    if (opt_target) {
        /* -t DIR: all remaining args are sources */
        if (i >= argc) {
            err_usage("install", "-t DIR SRC...");
            return 1;
        }

        /* Resolve owner/group */
        uid_t install_uid = (uid_t)-1;
        gid_t install_gid = (gid_t)-1;
        if (opt_owner && resolve_uid(opt_owner, &install_uid) != 0) {
            err_msg("install", "invalid user '%s'", opt_owner);
            return 1;
        }
        if (opt_group && resolve_gid(opt_group, &install_gid) != 0) {
            err_msg("install", "invalid group '%s'", opt_group);
            return 1;
        }

        for (; i < argc; i++) {
            const char *src = argv[i];
            const char *base = path_basename(src);
            char dst[PATH_MAX];
            if (!path_join(opt_target, base, dst)) {
                err_msg("install", "path too long");
                ret = 1;
                continue;
            }
            if (opt_suffix && make_backup(dst, opt_suffix) != 0) {
                ret = 1; continue;
            }
            if (opt_v)
                printf("install: '%s' -> '%s'\n", src, dst);
            if (copy_file(src, dst, opt_m, opt_p) != 0) {
                ret = 1; continue;
            }
            if (opt_s && run_strip(dst) != 0) {
                ret = 1; continue;
            }
            /* chmod to exact mode */
            chmod(dst, opt_m);
            /* chown (best effort; may fail without privilege) */
            if (install_uid != (uid_t)-1 || install_gid != (gid_t)-1) {
                if (chown(dst, install_uid, install_gid) != 0) { /* best effort */ }
            }
        }
        return ret;
    }

    /* Standard: SRC... DST */
    int nargs = argc - i;
    if (nargs < 2) {
        err_usage("install", "[-dvps] [-m MODE] [-o OWNER] [-g GROUP] SRC... DST");
        return 1;
    }

    const char *dst_arg = argv[argc - 1];
    int nsrc = nargs - 1;

    /* Resolve owner/group */
    uid_t install_uid = (uid_t)-1;
    gid_t install_gid = (gid_t)-1;
    if (opt_owner && resolve_uid(opt_owner, &install_uid) != 0) {
        err_msg("install", "invalid user '%s'", opt_owner);
        return 1;
    }
    if (opt_group && resolve_gid(opt_group, &install_gid) != 0) {
        err_msg("install", "invalid group '%s'", opt_group);
        return 1;
    }

    /* Check if dst is an existing directory */
    struct stat dst_st;
    int dst_is_dir = (stat(dst_arg, &dst_st) == 0 && S_ISDIR(dst_st.st_mode));

    if (nsrc > 1 && !dst_is_dir) {
        err_msg("install", "target '%s' is not a directory", dst_arg);
        return 1;
    }

    for (int j = 0; j < nsrc; j++) {
        const char *src = argv[i + j];
        char dst[PATH_MAX];

        if (dst_is_dir) {
            const char *base = path_basename(src);
            if (!path_join(dst_arg, base, dst)) {
                err_msg("install", "path too long");
                ret = 1;
                continue;
            }
        } else {
            size_t len = strlen(dst_arg);
            if (len >= PATH_MAX) {
                err_msg("install", "path too long");
                ret = 1;
                continue;
            }
            memcpy(dst, dst_arg, len + 1);
        }

        if (opt_suffix && make_backup(dst, opt_suffix) != 0) {
            ret = 1; continue;
        }
        if (opt_v)
            printf("install: '%s' -> '%s'\n", src, dst);
        if (copy_file(src, dst, opt_m, opt_p) != 0) {
            ret = 1; continue;
        }
        if (opt_s && run_strip(dst) != 0) {
            ret = 1; continue;
        }
        chmod(dst, opt_m);
        if (install_uid != (uid_t)-1 || install_gid != (gid_t)-1) {
            if (chown(dst, install_uid, install_gid) != 0) { /* best effort */ }
        }
    }

    return ret;
}
