/* chmod.c — chmod builtin: change file mode bits */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* nftw/FTW_PHYS require _XOPEN_SOURCE >= 500; S_ISVTX requires _DEFAULT_SOURCE */
#define _XOPEN_SOURCE 700
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../util/error.h"
#include "../util/path.h"
#include "../cache/fscache.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Global state for nftw callback (nftw does not pass a user pointer).
 * ------------------------------------------------------------------------- */
static mode_t  g_ref_mode;      /* mode from --reference, or sentinel */
static int     g_verbose;
static int     g_any_error;     /* set to 1 if any chmod fails */
static char    g_modestr[256];  /* raw mode string for symbolic parsing */
static int     g_use_ref;       /* 1 if --reference was supplied */

/* -------------------------------------------------------------------------
 * parse_symbolic_clause
 *
 * Parse a single comma-less symbolic clause of the form:
 *   [ugoa]*[+-=][rwxXstugo]+
 *
 * current: current mode of the file (used for 'X' logic).
 * is_dir : 1 if the path is a directory (used for 'X' logic).
 *
 * Returns the new mode, or (mode_t)-1 on parse error.
 * ------------------------------------------------------------------------- */
static mode_t parse_symbolic_clause(const char *clause, mode_t current, int is_dir)
{
    mode_t result = current;
    const char *p = clause;

    /* --- who --- */
    /* which of user/group/other this clause applies to */
    int who_u = 0, who_g = 0, who_o = 0;
    int who_all = 0;

    while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
        if (*p == 'u') who_u = 1;
        else if (*p == 'g') who_g = 1;
        else if (*p == 'o') who_o = 1;
        else if (*p == 'a') who_all = 1;
        p++;
    }
    /* Default: 'a' (all), but umask is applied – we match GNU: default is 'a' */
    if (!who_u && !who_g && !who_o && !who_all) {
        who_all = 1;
    }
    if (who_all) {
        who_u = who_g = who_o = 1;
    }

    /* --- operator --- */
    char op = *p;
    if (op != '+' && op != '-' && op != '=') {
        return (mode_t)-1;
    }
    p++;

    /* --- permission bits --- */
    /* Build a per-category permission mask from the permissions string */
    /* We compute the bits for one "unit" (user), then shift for group/other */
    mode_t u_bits = 0; /* bits to apply in user position */
    mode_t g_bits = 0;
    mode_t o_bits = 0;
    mode_t special = 0; /* setuid / setgid / sticky */

    /* 'X': execute/search only if directory or any exec bit already set */
    int any_exec = (current & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    int do_X = is_dir || any_exec;

    while (*p) {
        switch (*p) {
        case 'r':
            if (who_u) u_bits |= S_IRUSR;
            if (who_g) g_bits |= S_IRGRP;
            if (who_o) o_bits |= S_IROTH;
            break;
        case 'w':
            if (who_u) u_bits |= S_IWUSR;
            if (who_g) g_bits |= S_IWGRP;
            if (who_o) o_bits |= S_IWOTH;
            break;
        case 'x':
            if (who_u) u_bits |= S_IXUSR;
            if (who_g) g_bits |= S_IXGRP;
            if (who_o) o_bits |= S_IXOTH;
            break;
        case 'X':
            if (do_X) {
                if (who_u) u_bits |= S_IXUSR;
                if (who_g) g_bits |= S_IXGRP;
                if (who_o) o_bits |= S_IXOTH;
            }
            break;
        case 's':
            /* setuid if who includes u; setgid if who includes g */
            if (who_u) special |= S_ISUID;
            if (who_g) special |= S_ISGID;
            break;
        case 't':
            special |= S_ISVTX;
            break;
        /* Source permissions: copy from current mode */
        case 'u':
            /* Copy user bits to relevant positions */
            {
                mode_t src_r = (current & S_IRUSR) ? 1 : 0;
                mode_t src_w = (current & S_IWUSR) ? 1 : 0;
                mode_t src_x = (current & S_IXUSR) ? 1 : 0;
                if (who_u) {
                    if (src_r) u_bits |= S_IRUSR;
                    if (src_w) u_bits |= S_IWUSR;
                    if (src_x) u_bits |= S_IXUSR;
                }
                if (who_g) {
                    if (src_r) g_bits |= S_IRGRP;
                    if (src_w) g_bits |= S_IWGRP;
                    if (src_x) g_bits |= S_IXGRP;
                }
                if (who_o) {
                    if (src_r) o_bits |= S_IROTH;
                    if (src_w) o_bits |= S_IWOTH;
                    if (src_x) o_bits |= S_IXOTH;
                }
            }
            break;
        case 'g':
            {
                mode_t src_r = (current & S_IRGRP) ? 1 : 0;
                mode_t src_w = (current & S_IWGRP) ? 1 : 0;
                mode_t src_x = (current & S_IXGRP) ? 1 : 0;
                if (who_u) {
                    if (src_r) u_bits |= S_IRUSR;
                    if (src_w) u_bits |= S_IWUSR;
                    if (src_x) u_bits |= S_IXUSR;
                }
                if (who_g) {
                    if (src_r) g_bits |= S_IRGRP;
                    if (src_w) g_bits |= S_IWGRP;
                    if (src_x) g_bits |= S_IXGRP;
                }
                if (who_o) {
                    if (src_r) o_bits |= S_IROTH;
                    if (src_w) o_bits |= S_IWOTH;
                    if (src_x) o_bits |= S_IXOTH;
                }
            }
            break;
        case 'o':
            {
                mode_t src_r = (current & S_IROTH) ? 1 : 0;
                mode_t src_w = (current & S_IWOTH) ? 1 : 0;
                mode_t src_x = (current & S_IXOTH) ? 1 : 0;
                if (who_u) {
                    if (src_r) u_bits |= S_IRUSR;
                    if (src_w) u_bits |= S_IWUSR;
                    if (src_x) u_bits |= S_IXUSR;
                }
                if (who_g) {
                    if (src_r) g_bits |= S_IRGRP;
                    if (src_w) g_bits |= S_IWGRP;
                    if (src_x) g_bits |= S_IXGRP;
                }
                if (who_o) {
                    if (src_r) o_bits |= S_IROTH;
                    if (src_w) o_bits |= S_IWOTH;
                    if (src_x) o_bits |= S_IXOTH;
                }
            }
            break;
        default:
            return (mode_t)-1;
        }
        p++;
    }

    mode_t change_bits = u_bits | g_bits | o_bits | special;

    switch (op) {
    case '+':
        result = current | change_bits;
        break;
    case '-':
        result = current & ~change_bits;
        break;
    case '=':
        /* Clear all affected bits, then set new ones */
        {
            mode_t clear_mask = 0;
            if (who_u) clear_mask |= S_IRUSR | S_IWUSR | S_IXUSR | S_ISUID;
            if (who_g) clear_mask |= S_IRGRP | S_IWGRP | S_IXGRP | S_ISGID;
            if (who_o) clear_mask |= S_IROTH | S_IWOTH | S_IXOTH;
            /* sticky is cleared only if 'a' or 'o' in who */
            if (who_o) clear_mask |= S_ISVTX;
            result = (current & ~clear_mask) | change_bits;
        }
        break;
    }

    return result;
}

/* -------------------------------------------------------------------------
 * parse_mode
 *
 * Convert a mode string to an absolute mode_t given the file's current mode.
 * Returns (mode_t)-1 on error.
 * ------------------------------------------------------------------------- */
static mode_t parse_mode(const char *modestr, mode_t current, int is_dir)
{
    /* Octal: starts with a digit */
    if (*modestr >= '0' && *modestr <= '9') {
        char *end;
        unsigned long val = strtoul(modestr, &end, 8);
        if (*end != '\0' || val > 07777) {
            err_msg("chmod", "invalid mode: '%s'", modestr);
            return (mode_t)-1;
        }
        return (mode_t)val;
    }

    /* Also accept octal without leading zero (e.g. "755") if all digits 0-7 */
    {
        int looks_octal = 1;
        const char *q = modestr;
        while (*q) {
            if (*q < '0' || *q > '7') { looks_octal = 0; break; }
            q++;
        }
        if (looks_octal && q != modestr) {
            char *end;
            unsigned long val = strtoul(modestr, &end, 8);
            if (*end == '\0' && val <= 07777)
                return (mode_t)val;
        }
    }

    /* Symbolic: split on commas, apply each clause in sequence */
    char copy[256];
    size_t mlen = strlen(modestr);
    if (mlen >= sizeof(copy)) {
        err_msg("chmod", "mode string too long");
        return (mode_t)-1;
    }
    memcpy(copy, modestr, mlen + 1);

    mode_t result = current;
    char *tok = copy;
    char *next;

    while (tok && *tok) {
        next = strchr(tok, ',');
        if (next) *next = '\0';

        mode_t m = parse_symbolic_clause(tok, result, is_dir);
        if (m == (mode_t)-1) {
            err_msg("chmod", "invalid mode: '%s'", modestr);
            return (mode_t)-1;
        }
        result = m;

        tok = next ? next + 1 : NULL;
    }

    return result;
}

/* -------------------------------------------------------------------------
 * do_chmod: apply chmod to a single path.
 *
 * If g_use_ref is set, g_ref_mode is used directly.
 * Otherwise, the mode is parsed from g_modestr against the file's current mode.
 * ------------------------------------------------------------------------- */
static void do_chmod(const char *path, const struct stat *st)
{
    mode_t new_mode;

    if (g_use_ref) {
        new_mode = g_ref_mode & 07777;
    } else {
        int is_dir = S_ISDIR(st->st_mode);
        new_mode = parse_mode(g_modestr, st->st_mode & 07777, is_dir);
        if (new_mode == (mode_t)-1) {
            g_any_error = 1;
            return;
        }
    }

    int rc;

    if (S_ISLNK(st->st_mode)) {
        /* Try fchmodat with AT_SYMLINK_NOFOLLOW; skip if not supported */
#ifdef AT_SYMLINK_NOFOLLOW
        rc = fchmodat(AT_FDCWD, path, new_mode, AT_SYMLINK_NOFOLLOW);
        if (rc != 0 && errno == ENOTSUP) {
            /* Kernel/fs doesn't support it; skip silently */
            return;
        }
#else
        /* No support; skip symlinks */
        return;
#endif
    } else {
        rc = chmod(path, new_mode);
    }

    if (rc != 0) {
        err_sys("chmod", "changing permissions of '%s'", path);
        g_any_error = 1;
        return;
    }

    /* B-7: update fscache entry with fresh stat (written_by_matchbox=1) */
    {
        struct stat fresh;
        if (stat(path, &fresh) == 0)
            fscache_insert(path, &fresh);
    }

    if (g_verbose) {
        printf("mode of '%s' changed to %04o\n", path, (unsigned)new_mode);
    }
}

/* -------------------------------------------------------------------------
 * nftw callback for -R recursive mode.
 * We use FTW_PHYS so symlinks are never automatically followed.
 * ------------------------------------------------------------------------- */
static int nftw_cb(const char *path, const struct stat *st,
                   int typeflag, struct FTW *ftwbuf)
{
    (void)typeflag;
    (void)ftwbuf;
    do_chmod(path, st);
    return 0; /* continue traversal even on error */
}

/* -------------------------------------------------------------------------
 * applet_chmod
 * ------------------------------------------------------------------------- */
int applet_chmod(int argc, char **argv)
{
    int opt_recursive = 0;
    int opt_verbose   = 0;
    int have_mode     = 0;
    char ref_path[PATH_MAX];
    int  have_ref     = 0;
    int ret = 0;
    int i;

    g_any_error  = 0;
    g_use_ref    = 0;
    g_verbose    = 0;
    g_ref_mode   = 0;
    g_modestr[0] = '\0';

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Long options */
        if (strcmp(arg, "--recursive") == 0) {
            opt_recursive = 1;
            continue;
        }
        if (strcmp(arg, "--verbose") == 0) {
            opt_verbose = 1;
            continue;
        }
        if (strncmp(arg, "--reference=", 12) == 0) {
            const char *rfile = arg + 12;
            size_t rlen = strlen(rfile);
            if (rlen == 0 || rlen >= PATH_MAX) {
                err_msg("chmod", "--reference path invalid");
                return 1;
            }
            memcpy(ref_path, rfile, rlen + 1);
            have_ref = 1;
            continue;
        }

        /* Short flags */
        const char *p = arg + 1;
        while (*p) {
            switch (*p) {
            case 'R': opt_recursive = 1; break;
            case 'v': opt_verbose   = 1; break;
            default:
                err_msg("chmod", "unrecognized option '-%c'", *p);
                err_usage("chmod",
                    "[-Rv] [--reference=RFILE] MODE FILE...");
                return 1;
            }
            p++;
        }
    }

    g_verbose = opt_verbose;

    /* Resolve --reference */
    if (have_ref) {
        struct stat ref_st;
        if (stat(ref_path, &ref_st) != 0) {
            err_sys("chmod", "failed to stat reference '%s'", ref_path);
            return 1;
        }
        g_ref_mode = ref_st.st_mode;
        g_use_ref  = 1;
    } else {
        /* Next argument must be the mode string */
        if (i >= argc) {
            err_usage("chmod", "[-Rv] [--reference=RFILE] MODE FILE...");
            return 1;
        }
        size_t mlen = strlen(argv[i]);
        if (mlen >= sizeof(g_modestr)) {
            err_msg("chmod", "mode string too long");
            return 1;
        }
        memcpy(g_modestr, argv[i], mlen + 1);
        have_mode = 1;
        i++;
    }

    if (!have_ref && !have_mode) {
        err_usage("chmod", "[-Rv] [--reference=RFILE] MODE FILE...");
        return 1;
    }

    if (i >= argc) {
        err_usage("chmod", "[-Rv] [--reference=RFILE] MODE FILE...");
        return 1;
    }

    for (; i < argc; i++) {
        const char *path = argv[i];

        if (opt_recursive) {
            /*
             * nftw with FTW_PHYS: do not follow symlinks.
             * FTW_DEPTH: post-order (children before parent) is not needed
             * for chmod, but we use pre-order (default) which is fine.
             */
            int nftw_flags = FTW_PHYS;
            if (nftw(path, nftw_cb, 16, nftw_flags) != 0) {
                err_sys("chmod", "error traversing '%s'", path);
                ret = 1;
            }
        } else {
            struct stat st;
            if (lstat(path, &st) != 0) {
                err_sys("chmod", "cannot access '%s'", path);
                ret = 1;
                continue;
            }
            do_chmod(path, &st);
        }
    }

    if (g_any_error)
        ret = 1;

    return ret;
}
