/* ln.c — ln builtin: create hard or symbolic links */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"
#include "../util/strbuf.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct ln_opts {
    int symbolic;      /* -s */
    int force;         /* -f */
    int verbose;       /* -v */
    int no_deref;      /* -n: treat dst symlink as file, not as directory */
    int relative;      /* -r */
    int no_target;     /* -T */
};

/* -------------------------------------------------------------------------
 * compute_relative_path
 *
 * Compute the relative path from `from_dir` to `target`, such that
 * `from_dir/<result>` resolves to `target`.
 *
 * Both paths should be absolute (or at minimum consistent).
 * The result is written into `out` (PATH_MAX bytes).
 * Returns out on success, NULL on overflow.
 * ------------------------------------------------------------------------- */
static char *compute_relative_path(const char *from_dir, const char *target,
                                   char out[PATH_MAX])
{
    /*
     * Algorithm:
     * 1. Find the longest common prefix (at a '/' boundary).
     * 2. Count how many components remain in from_dir after the common prefix
     *    -> that many "../" prefixes are needed.
     * 3. Append the remaining components of target after the common prefix.
     */

    /* Work with copies so we can tokenise */
    char fd_buf[PATH_MAX];
    char tg_buf[PATH_MAX];

    size_t fdlen = strlen(from_dir);
    size_t tglen = strlen(target);

    if (fdlen >= PATH_MAX || tglen >= PATH_MAX) {
        err_msg("ln", "path too long computing relative path");
        return NULL;
    }

    memcpy(fd_buf, from_dir, fdlen + 1);
    memcpy(tg_buf, target,   tglen + 1);

    /* Tokenise both paths into components */
    char *fd_parts[PATH_MAX / 2];
    char *tg_parts[PATH_MAX / 2];
    int   fd_n = 0, tg_n = 0;

    /* Parse from_dir */
    for (char *tok = fd_buf; ; ) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok != '\0') {
            if (fd_n >= (int)(PATH_MAX / 2) - 1) {
                err_msg("ln", "too many path components");
                return NULL;
            }
            fd_parts[fd_n++] = tok;
        }
        if (!slash) break;
        tok = slash + 1;
    }

    /* Parse target */
    for (char *tok = tg_buf; ; ) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok != '\0') {
            if (tg_n >= (int)(PATH_MAX / 2) - 1) {
                err_msg("ln", "too many path components");
                return NULL;
            }
            tg_parts[tg_n++] = tok;
        }
        if (!slash) break;
        tok = slash + 1;
    }

    /* Find common prefix length */
    int common = 0;
    int limit = (fd_n < tg_n) ? fd_n : tg_n;
    while (common < limit && strcmp(fd_parts[common], tg_parts[common]) == 0)
        common++;

    /* Build the relative path using a strbuf */
    strbuf_t sb;
    if (sb_init(&sb, PATH_MAX) < 0) {
        err_msg("ln", "out of memory");
        return NULL;
    }

    /* One "../" for each remaining component in from_dir */
    int ups = fd_n - common;
    for (int k = 0; k < ups; k++) {
        if (sb_append(&sb, "../") < 0) {
            sb_free(&sb);
            err_msg("ln", "path too long computing relative path");
            return NULL;
        }
    }

    /* Append remaining components of target */
    for (int k = common; k < tg_n; k++) {
        if (k > common) {
            if (sb_appendc(&sb, '/') < 0) {
                sb_free(&sb);
                err_msg("ln", "path too long computing relative path");
                return NULL;
            }
        }
        if (sb_append(&sb, tg_parts[k]) < 0) {
            sb_free(&sb);
            err_msg("ln", "path too long computing relative path");
            return NULL;
        }
    }

    /* If empty (same directory), use the basename of target */
    if (sb_len(&sb) == 0) {
        if (sb_append(&sb, path_basename(target)) < 0) {
            sb_free(&sb);
            err_msg("ln", "path too long computing relative path");
            return NULL;
        }
    }

    if (sb_len(&sb) >= PATH_MAX) {
        sb_free(&sb);
        err_msg("ln", "relative path too long");
        return NULL;
    }

    memcpy(out, sb_str(&sb), sb_len(&sb) + 1);
    sb_free(&sb);
    return out;
}

/* -------------------------------------------------------------------------
 * do_ln: create a link from src (or link_target for symlinks) to dst.
 *
 * For symlinks:
 *   - If -r, compute relative path from dirname(dst) to src and use that.
 *   - Otherwise use src literally as the symlink target.
 * For hard links:
 *   - link(src, dst)
 *
 * Returns 0 on success, 1 on error.
 * ------------------------------------------------------------------------- */
static int do_ln(const char *src, const char *dst, const struct ln_opts *opts)
{
    /* -f: remove dst if it exists */
    if (opts->force) {
        /*
         * Determine whether dst is an existing symlink or file.
         * With -n, a symlink at dst is treated as a file (not a directory
         * to enter).  In either case we want to unlink it.
         */
        struct stat dst_st;
        if (lstat(dst, &dst_st) == 0) {
            if (unlink(dst) != 0) {
                err_sys("ln", "cannot remove '%s'", dst);
                return 1;
            }
        }
    }

    int rc;

    if (opts->symbolic) {
        const char *link_target;
        char rel_buf[PATH_MAX];

        if (opts->relative) {
            /* Compute relative symlink target */
            char dst_dir[PATH_MAX];
            path_dirname(dst, dst_dir);

            /* Canonicalize dst_dir so we can do proper relative computation */
            char dst_dir_abs[PATH_MAX];
            char src_abs[PATH_MAX];

            /* If paths are already absolute use them; otherwise resolve */
            if (src[0] == '/') {
                if (strlen(src) >= PATH_MAX) {
                    err_msg("ln", "path too long");
                    return 1;
                }
                memcpy(src_abs, src, strlen(src) + 1);
            } else {
                /* Make src absolute relative to cwd */
                if (getcwd(src_abs, sizeof(src_abs)) == NULL) {
                    err_sys("ln", "cannot get working directory");
                    return 1;
                }
                char joined[PATH_MAX];
                if (!path_join(src_abs, src, joined)) {
                    err_msg("ln", "path too long");
                    return 1;
                }
                memcpy(src_abs, joined, strlen(joined) + 1);
            }

            if (dst_dir[0] == '/') {
                if (strlen(dst_dir) >= PATH_MAX) {
                    err_msg("ln", "path too long");
                    return 1;
                }
                memcpy(dst_dir_abs, dst_dir, strlen(dst_dir) + 1);
            } else {
                if (getcwd(dst_dir_abs, sizeof(dst_dir_abs)) == NULL) {
                    err_sys("ln", "cannot get working directory");
                    return 1;
                }
                char joined[PATH_MAX];
                if (!path_join(dst_dir_abs, dst_dir, joined)) {
                    err_msg("ln", "path too long");
                    return 1;
                }
                memcpy(dst_dir_abs, joined, strlen(joined) + 1);
            }

            if (!compute_relative_path(dst_dir_abs, src_abs, rel_buf))
                return 1;
            link_target = rel_buf;
        } else {
            link_target = src;
        }

        rc = symlink(link_target, dst);
        if (rc != 0) {
            err_sys("ln", "cannot create symlink '%s' -> '%s'", dst, link_target);
            return 1;
        }

        if (opts->verbose)
            printf("'%s' -> '%s'\n", dst, link_target);
    } else {
        /* Hard link */
        rc = link(src, dst);
        if (rc != 0) {
            err_sys("ln", "cannot create hard link '%s' -> '%s'", dst, src);
            return 1;
        }

        if (opts->verbose)
            printf("'%s' => '%s'\n", dst, src);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * resolve_link_dest
 *
 * Determine the actual path at which to create the link for source `src`
 * given destination `dst`.
 *
 * Rules:
 *   - If -T: use dst as-is.
 *   - If dst is an existing directory (and not -n treating it as file):
 *       append basename(src).
 *   - Otherwise: use dst as-is.
 *
 * result must be PATH_MAX bytes.
 * Returns result on success, NULL on overflow.
 * ------------------------------------------------------------------------- */
static char *resolve_link_dest(const char *src, const char *dst,
                                int no_target, int no_deref,
                                char result[PATH_MAX])
{
    if (no_target) {
        size_t dlen = strlen(dst);
        if (dlen >= PATH_MAX) return NULL;
        memcpy(result, dst, dlen + 1);
        return result;
    }

    struct stat dst_st;
    int dst_is_dir = 0;

    if (!no_deref) {
        /* Normal: follow symlink at dst to check if it's a directory */
        if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode))
            dst_is_dir = 1;
    } else {
        /* -n: treat a symlink at dst as a regular file, not a directory.
         * Use lstat; if it's a symlink we don't enter it.  If it's a
         * real directory (not a symlink to a dir), we still enter it. */
        if (lstat(dst, &dst_st) == 0 &&
            S_ISDIR(dst_st.st_mode) &&
            !S_ISLNK(dst_st.st_mode))
            dst_is_dir = 1;
    }

    if (dst_is_dir) {
        const char *base = path_basename(src);
        if (!path_join(dst, base, result))
            return NULL;
        return result;
    }

    size_t dlen = strlen(dst);
    if (dlen >= PATH_MAX) return NULL;
    memcpy(result, dst, dlen + 1);
    return result;
}

/* -------------------------------------------------------------------------
 * applet_ln
 * ------------------------------------------------------------------------- */
int applet_ln(int argc, char **argv)
{
    struct ln_opts opts = {0};
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Long options */
        if (strcmp(arg, "--symbolic") == 0)          { opts.symbolic = 1;  continue; }
        if (strcmp(arg, "--force") == 0)              { opts.force = 1;     continue; }
        if (strcmp(arg, "--verbose") == 0)            { opts.verbose = 1;   continue; }
        if (strcmp(arg, "--no-dereference") == 0)     { opts.no_deref = 1;  continue; }
        if (strcmp(arg, "--relative") == 0)           { opts.relative = 1;  continue; }
        if (strcmp(arg, "--no-target-directory") == 0){ opts.no_target = 1; continue; }

        /* Short flags */
        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 's': opts.symbolic = 1;  break;
            case 'f': opts.force    = 1;  break;
            case 'v': opts.verbose  = 1;  break;
            case 'n': opts.no_deref = 1;  break;
            case 'r': opts.relative = 1;  break;
            case 'T': opts.no_target = 1; break;
            default:
                err_msg("ln", "unrecognized option '-%c'", *p);
                err_usage("ln", "[-sfvnrT] TARGET LINK_NAME");
                return 1;
            }
            p++;
        }
    }

    int noperands = argc - i;
    if (noperands < 2) {
        err_usage("ln", "[-sfvnrT] TARGET... LINK_NAME");
        return 1;
    }

    const char *dst = argv[argc - 1];
    int nsrc = noperands - 1;

    /* Determine if dst is an existing directory */
    struct stat dst_st;
    int dst_exists  = (lstat(dst, &dst_st) == 0);
    int dst_is_real_dir = dst_exists && S_ISDIR(dst_st.st_mode)
                          && !S_ISLNK(dst_st.st_mode);

    /* Multiple sources require dst to be (or become) a directory */
    if (nsrc > 1 && !dst_is_real_dir) {
        if (dst_exists)
            fprintf(stderr, "ln: target '%s' is not a directory\n", dst);
        else
            fprintf(stderr, "ln: target directory '%s': "
                            "No such file or directory\n", dst);
        return 1;
    }

    if (opts.no_target && dst_is_real_dir) {
        fprintf(stderr, "ln: cannot overwrite directory '%s'\n", dst);
        return 1;
    }

    for (int j = i; j < argc - 1; j++) {
        const char *src = argv[j];
        char real_dst[PATH_MAX];

        if (!resolve_link_dest(src, dst, opts.no_target, opts.no_deref,
                               real_dst)) {
            err_msg("ln", "path too long");
            ret = 1;
            continue;
        }

        if (do_ln(src, real_dst, &opts) != 0)
            ret = 1;
    }

    return ret;
}
