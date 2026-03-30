/* find.c — find builtin: traverse directory trees and apply tests */

/* _GNU_SOURCE enables strcasestr and FNM_CASEFOLD */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"
#include "../util/strbuf.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Predicate / expression tree                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    PRED_TRUE = 0,
    PRED_NAME,
    PRED_INAME,
    PRED_TYPE,
    PRED_PRINT,
    PRED_PRINT0,
    PRED_DELETE,
    PRED_EMPTY,
    PRED_NEWER,
    PRED_MTIME,
    PRED_ATIME,
    PRED_SIZE,
    PRED_PERM,
    PRED_USER,
    PRED_GROUP,
    PRED_EXEC,       /* -exec ... ; */
    PRED_EXEC_PLUS,  /* -exec ... + */
    PRED_MAXDEPTH,   /* not a predicate, stored separately */
    PRED_MINDEPTH,
    PRED_AND,
    PRED_OR,
    PRED_NOT,
} pred_type_t;

/* Size unit */
typedef enum { SZ_BYTES=0, SZ_KILO, SZ_MEGA, SZ_GIGA, SZ_BLOCKS } size_unit_t;

/* Time comparison: +N means > N, -N means < N, N means == N */
typedef struct {
    int  sign;  /* -1, 0, +1 */
    long val;
} cmp_t;

/* Compiled fast-path for -name/-iname patterns (O-10) */
typedef enum {
    CGLOB_FULL = 0,  /* fallback to fnmatch */
    CGLOB_LITERAL,   /* strcmp / strcasecmp */
    CGLOB_SUFFIX,    /* ends-with: fixed = pattern+1 */
    CGLOB_PREFIX,    /* starts-with: fixed = pattern (trailing * zeroed) */
    CGLOB_CONTAINS   /* substring: fixed = pattern+1 (trailing * zeroed) */
} cglob_type_t;

typedef struct expr_node {
    pred_type_t  type;

    /* PRED_NAME / PRED_INAME */
    char        *pattern;
    /* compiled fast-path; populated by compile_glob() after parse */
    struct {
        cglob_type_t type;
        const char  *fixed; /* NUL-terminated fixed part within pattern buffer */
        size_t       flen;
    } cglob;

    /* PRED_TYPE */
    char         ftype; /* f/d/l/b/c/p/s */

    /* PRED_NEWER */
    time_t       newer_mtime;

    /* PRED_MTIME / PRED_ATIME */
    cmp_t        time_cmp;

    /* PRED_SIZE */
    cmp_t        size_cmp;
    size_unit_t  size_unit;

    /* PRED_PERM */
    mode_t       perm_mode;
    int          perm_exact; /* 1: exact match, 0: at least */

    /* PRED_USER */
    uid_t        uid;

    /* PRED_GROUP */
    gid_t        gid;

    /* PRED_EXEC / PRED_EXEC_PLUS */
    char       **exec_argv;  /* NULL-terminated template */
    int          exec_argc;
    int          exec_plus_braces_idx; /* index of {} in exec_argv for + form */

    /* Logical: children */
    struct expr_node *left;
    struct expr_node *right;
} expr_node_t;

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char **argv;
    int    argc;
    int    pos;
    int    maxdepth;
    int    mindepth;
    int    follow_symlinks; /* -L */
    int    has_action;      /* did user specify -print, -exec, -delete, etc. */
} find_args_t;

/* ------------------------------------------------------------------ */
/* Expression tree constructors                                       */
/* ------------------------------------------------------------------ */

/*
 * compile_glob: classify a -name/-iname pattern for fast matching.
 * Modifies n->pattern in-place ONLY for PREFIX and CONTAINS cases
 * (zeroes the trailing '*').  Must be called after strdup'ing the pattern.
 */
static void compile_glob(expr_node_t *n)
{
    if (!n->pattern) return;
    const char *p = n->pattern;
    size_t len = strlen(p);
    size_t star_count = 0, star1 = 0, star2 = 0;
    int has_other = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        if (p[i] == '*') {
            if (star_count == 0) star1 = i;
            else if (star_count == 1) star2 = i;
            star_count++;
        } else if (p[i] == '?' || p[i] == '[') {
            has_other = 1;
        }
    }

    if (!star_count && !has_other) {
        n->cglob.type  = CGLOB_LITERAL;
        n->cglob.fixed = p;
        n->cglob.flen  = len;
        return;
    }
    if (has_other || star_count > 2) {
        n->cglob.type = CGLOB_FULL;
        return;
    }
    if (star_count == 1) {
        if (star1 == 0) {
            /* *.ext */
            n->cglob.type  = CGLOB_SUFFIX;
            n->cglob.fixed = p + 1;
            n->cglob.flen  = len - 1;
        } else if (star1 == len - 1) {
            /* prefix* — zero trailing star */
            n->pattern[len - 1] = '\0';
            n->cglob.type  = CGLOB_PREFIX;
            n->cglob.fixed = n->pattern;
            n->cglob.flen  = len - 1;
        } else {
            n->cglob.type = CGLOB_FULL;
        }
        return;
    }
    /* star_count == 2 */
    if (star1 == 0 && star2 == len - 1) {
        /* *substr* — zero trailing star */
        n->pattern[len - 1] = '\0';
        n->cglob.type  = CGLOB_CONTAINS;
        n->cglob.fixed = n->pattern + 1;
        n->cglob.flen  = len - 2;
    } else {
        n->cglob.type = CGLOB_FULL;
    }
}

static expr_node_t *node_new(pred_type_t type)
{
    expr_node_t *n = calloc(1, sizeof(expr_node_t));
    if (!n)
        err_msg("find", "out of memory");
    else
        n->type = type;
    return n;
}

static void node_free(expr_node_t *n)
{
    if (!n) return;
    node_free(n->left);
    node_free(n->right);
    free(n->pattern);
    if (n->exec_argv) {
        for (int i = 0; i < n->exec_argc; i++)
            free(n->exec_argv[i]);
        free(n->exec_argv);
    }
    free(n);
}

/* ------------------------------------------------------------------ */
/* Parsing helpers                                                    */
/* ------------------------------------------------------------------ */

static const char *peek_arg(find_args_t *fa)
{
    if (fa->pos < fa->argc)
        return fa->argv[fa->pos];
    return NULL;
}

static const char *next_arg(find_args_t *fa)
{
    if (fa->pos < fa->argc)
        return fa->argv[fa->pos++];
    return NULL;
}

static cmp_t parse_cmp(const char *s)
{
    cmp_t c = {0, 0};
    if (*s == '+')      { c.sign = 1;  s++; }
    else if (*s == '-') { c.sign = -1; s++; }
    else                { c.sign = 0; }
    c.val = strtol(s, NULL, 10);
    return c;
}

static int cmp_matches(cmp_t c, long val)
{
    if (c.sign < 0) return val < c.val;
    if (c.sign > 0) return val > c.val;
    return val == c.val;
}

/* Forward declaration for recursive parsing */
static expr_node_t *parse_expr(find_args_t *fa);
static expr_node_t *parse_primary(find_args_t *fa);

static expr_node_t *parse_primary(find_args_t *fa)
{
    const char *arg = peek_arg(fa);
    if (!arg) return NULL;

    /* Parentheses */
    if (strcmp(arg, "(") == 0) {
        next_arg(fa);
        expr_node_t *inner = parse_expr(fa);
        const char *close = peek_arg(fa);
        if (close && strcmp(close, ")") == 0)
            next_arg(fa);
        return inner;
    }

    /* Negation */
    if (strcmp(arg, "!") == 0 || strcmp(arg, "-not") == 0) {
        next_arg(fa);
        expr_node_t *n = node_new(PRED_NOT);
        if (!n) return NULL;
        n->left = parse_primary(fa);
        return n;
    }

    if (arg[0] != '-')
        return NULL; /* not a primary */

    next_arg(fa);

    if (strcmp(arg, "-true") == 0) {
        return node_new(PRED_TRUE);
    }

    if (strcmp(arg, "-print") == 0) {
        fa->has_action = 1;
        return node_new(PRED_PRINT);
    }

    if (strcmp(arg, "-print0") == 0) {
        fa->has_action = 1;
        return node_new(PRED_PRINT0);
    }

    if (strcmp(arg, "-delete") == 0) {
        fa->has_action = 1;
        return node_new(PRED_DELETE);
    }

    if (strcmp(arg, "-empty") == 0) {
        return node_new(PRED_EMPTY);
    }

    if (strcmp(arg, "-name") == 0) {
        const char *pat = next_arg(fa);
        if (!pat) { err_msg("find", "-name requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_NAME);
        if (!n) return NULL;
        n->pattern = strdup(pat);
        if (!n->pattern) { err_msg("find", "out of memory"); node_free(n); return NULL; }
        compile_glob(n);
        return n;
    }

    if (strcmp(arg, "-iname") == 0) {
        const char *pat = next_arg(fa);
        if (!pat) { err_msg("find", "-iname requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_INAME);
        if (!n) return NULL;
        n->pattern = strdup(pat);
        if (!n->pattern) { err_msg("find", "out of memory"); node_free(n); return NULL; }
        compile_glob(n);
        return n;
    }

    if (strcmp(arg, "-type") == 0) {
        const char *tstr = next_arg(fa);
        if (!tstr) { err_msg("find", "-type requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_TYPE);
        if (!n) return NULL;
        n->ftype = tstr[0];
        return n;
    }

    if (strcmp(arg, "-maxdepth") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-maxdepth requires argument"); return NULL; }
        fa->maxdepth = (int)strtol(val, NULL, 10);
        return node_new(PRED_TRUE); /* transparent */
    }

    if (strcmp(arg, "-mindepth") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-mindepth requires argument"); return NULL; }
        fa->mindepth = (int)strtol(val, NULL, 10);
        return node_new(PRED_TRUE);
    }

    if (strcmp(arg, "-newer") == 0) {
        const char *ref = next_arg(fa);
        if (!ref) { err_msg("find", "-newer requires argument"); return NULL; }
        struct stat st;
        if (stat(ref, &st) != 0) {
            err_sys("find", "-newer: cannot stat '%s'", ref);
            return NULL;
        }
        expr_node_t *n = node_new(PRED_NEWER);
        if (!n) return NULL;
        n->newer_mtime = st.st_mtime;
        return n;
    }

    if (strcmp(arg, "-mtime") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-mtime requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_MTIME);
        if (!n) return NULL;
        n->time_cmp = parse_cmp(val);
        return n;
    }

    if (strcmp(arg, "-atime") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-atime requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_ATIME);
        if (!n) return NULL;
        n->time_cmp = parse_cmp(val);
        return n;
    }

    if (strcmp(arg, "-size") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-size requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_SIZE);
        if (!n) return NULL;
        size_t vlen = strlen(val);
        /* Copy value; strip trailing unit character if present */
        char numstr[64];
        size_t copy_len = vlen < sizeof(numstr) ? vlen : sizeof(numstr) - 1;
        memcpy(numstr, val, copy_len);
        numstr[copy_len] = '\0';
        /* Determine unit from last character of original val */
        if (copy_len > 0 && isalpha((unsigned char)numstr[copy_len - 1])) {
            char unit = numstr[copy_len - 1];
            numstr[copy_len - 1] = '\0'; /* strip unit */
            switch (unit) {
            case 'c': n->size_unit = SZ_BYTES;  break;
            case 'k': n->size_unit = SZ_KILO;   break;
            case 'M': n->size_unit = SZ_MEGA;   break;
            case 'G': n->size_unit = SZ_GIGA;   break;
            default:  n->size_unit = SZ_BLOCKS; break;
            }
        } else {
            n->size_unit = SZ_BLOCKS;
        }
        n->size_cmp = parse_cmp(numstr);
        return n;
    }

    if (strcmp(arg, "-perm") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-perm requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_PERM);
        if (!n) return NULL;
        int exact = 1;
        const char *p = val;
        if (*p == '-') { exact = 0; p++; }
        if (*p == '/') { exact = 0; p++; }
        n->perm_exact = exact;
        char *end;
        n->perm_mode = (mode_t)strtoul(p, &end, 8);
        return n;
    }

    if (strcmp(arg, "-user") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-user requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_USER);
        if (!n) return NULL;
        struct passwd *pw = getpwnam(val);
        if (pw) {
            n->uid = pw->pw_uid;
        } else {
            /* Try numeric */
            n->uid = (uid_t)strtoul(val, NULL, 10);
        }
        return n;
    }

    if (strcmp(arg, "-group") == 0) {
        const char *val = next_arg(fa);
        if (!val) { err_msg("find", "-group requires argument"); return NULL; }
        expr_node_t *n = node_new(PRED_GROUP);
        if (!n) return NULL;
        struct group *gr = getgrnam(val);
        if (gr) {
            n->gid = gr->gr_gid;
        } else {
            n->gid = (gid_t)strtoul(val, NULL, 10);
        }
        return n;
    }

    if (strcmp(arg, "-exec") == 0 || strcmp(arg, "-execdir") == 0) {
        fa->has_action = 1;
        /* Collect args until \; or + */
        int cap = 16;
        char **av = malloc((size_t)cap * sizeof(char *));
        if (!av) { err_msg("find", "out of memory"); return NULL; }
        int ac = 0;
        int plus_form = 0;
        int braces_idx = -1;

        for (;;) {
            const char *a = next_arg(fa);
            if (!a) {
                err_msg("find", "-exec: missing terminating ; or +");
                for (int k = 0; k < ac; k++) free(av[k]);
                free(av);
                return NULL;
            }
            if (strcmp(a, ";") == 0) break;
            if (strcmp(a, "+") == 0) { plus_form = 1; break; }

            if (ac >= cap - 1) {
                cap *= 2;
                char **tmp = realloc(av, (size_t)cap * sizeof(char *));
                if (!tmp) {
                    err_msg("find", "out of memory");
                    for (int k = 0; k < ac; k++) free(av[k]);
                    free(av);
                    return NULL;
                }
                av = tmp;
            }
            if (strcmp(a, "{}") == 0)
                braces_idx = ac;
            av[ac] = strdup(a);
            if (!av[ac]) {
                err_msg("find", "out of memory");
                for (int k = 0; k < ac; k++) free(av[k]);
                free(av);
                return NULL;
            }
            ac++;
        }
        av[ac] = NULL;

        expr_node_t *n = node_new(plus_form ? PRED_EXEC_PLUS : PRED_EXEC);
        if (!n) {
            for (int k = 0; k < ac; k++) free(av[k]);
            free(av);
            return NULL;
        }
        n->exec_argv = av;
        n->exec_argc = ac;
        n->exec_plus_braces_idx = braces_idx;
        return n;
    }

    /* Unknown primary */
    err_msg("find", "unknown primary '%s'", arg);
    return NULL;
}

static expr_node_t *parse_and_expr(find_args_t *fa)
{
    expr_node_t *left = parse_primary(fa);
    if (!left) return NULL;

    for (;;) {
        const char *tok = peek_arg(fa);
        if (!tok) break;
        if (strcmp(tok, "-o") == 0 || strcmp(tok, "-or") == 0)  break;
        if (strcmp(tok, ")")  == 0)                              break;
        if (strcmp(tok, "-a") == 0 || strcmp(tok, "-and") == 0) {
            next_arg(fa);
        }
        expr_node_t *right = parse_primary(fa);
        if (!right) { node_free(left); return NULL; }
        expr_node_t *n = node_new(PRED_AND);
        if (!n) { node_free(left); node_free(right); return NULL; }
        n->left  = left;
        n->right = right;
        left = n;
    }
    return left;
}

static expr_node_t *parse_expr(find_args_t *fa)
{
    expr_node_t *left = parse_and_expr(fa);
    if (!left) return NULL;

    for (;;) {
        const char *tok = peek_arg(fa);
        if (!tok) break;
        if (strcmp(tok, "-o") != 0 && strcmp(tok, "-or") != 0) break;
        next_arg(fa);
        expr_node_t *right = parse_and_expr(fa);
        if (!right) { node_free(left); return NULL; }
        expr_node_t *n = node_new(PRED_OR);
        if (!n) { node_free(left); node_free(right); return NULL; }
        n->left  = left;
        n->right = right;
        left = n;
    }
    return left;
}

/* ------------------------------------------------------------------ */
/* Predicate evaluation                                               */
/* ------------------------------------------------------------------ */

/* Execute -exec CMD {} \; for one path */
static int exec_cmd_single(expr_node_t *n, const char *path)
{
    /* Build argv: replace {} with path */
    char **av = malloc((size_t)(n->exec_argc + 1) * sizeof(char *));
    if (!av) {
        err_msg("find", "out of memory");
        return 0;
    }
    for (int k = 0; k < n->exec_argc; k++) {
        if (strcmp(n->exec_argv[k], "{}") == 0) {
            av[k] = (char *)path; /* not owned */
        } else {
            av[k] = n->exec_argv[k];
        }
    }
    av[n->exec_argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        err_sys("find", "fork");
        free(av);
        return 0;
    }
    if (pid == 0) {
        execvp(av[0], av);
        err_sys("find", "exec: %s", av[0]);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    free(av);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
}

static int eval_expr(expr_node_t *n, const char *path, const char *basename_ptr,
                     const struct stat *st, int depth)
{
    if (!n) return 1;

    switch (n->type) {
    case PRED_TRUE:
        return 1;

    case PRED_AND:
        return eval_expr(n->left,  path, basename_ptr, st, depth) &&
               eval_expr(n->right, path, basename_ptr, st, depth);

    case PRED_OR:
        return eval_expr(n->left,  path, basename_ptr, st, depth) ||
               eval_expr(n->right, path, basename_ptr, st, depth);

    case PRED_NOT:
        return !eval_expr(n->left, path, basename_ptr, st, depth);

    case PRED_NAME: {
        const char *s = basename_ptr;
        size_t slen = strlen(s);
        switch (n->cglob.type) {
        case CGLOB_LITERAL:
            return strcmp(s, n->cglob.fixed) == 0;
        case CGLOB_SUFFIX:
            return slen >= n->cglob.flen &&
                   memcmp(s + slen - n->cglob.flen,
                          n->cglob.fixed, n->cglob.flen) == 0;
        case CGLOB_PREFIX:
            return slen >= n->cglob.flen &&
                   memcmp(s, n->cglob.fixed, n->cglob.flen) == 0;
        case CGLOB_CONTAINS:
            return strstr(s, n->cglob.fixed) != NULL;
        default:
            return fnmatch(n->pattern, s, 0) == 0;
        }
    }

    case PRED_INAME: {
        const char *s = basename_ptr;
        size_t slen = strlen(s);
        switch (n->cglob.type) {
        case CGLOB_LITERAL:
            return strcasecmp(s, n->cglob.fixed) == 0;
        case CGLOB_SUFFIX:
            return slen >= n->cglob.flen &&
                   strncasecmp(s + slen - n->cglob.flen,
                               n->cglob.fixed, n->cglob.flen) == 0;
        case CGLOB_PREFIX:
            return slen >= n->cglob.flen &&
                   strncasecmp(s, n->cglob.fixed, n->cglob.flen) == 0;
        case CGLOB_CONTAINS:
            return strcasestr(s, n->cglob.fixed) != NULL;
        default:
            return fnmatch(n->pattern, s, FNM_CASEFOLD) == 0;
        }
    }

    case PRED_TYPE: {
        mode_t m = st->st_mode;
        switch (n->ftype) {
        case 'f': return S_ISREG(m);
        case 'd': return S_ISDIR(m);
        case 'l': return S_ISLNK(m);
        case 'b': return S_ISBLK(m);
        case 'c': return S_ISCHR(m);
        case 'p': return S_ISFIFO(m);
        case 's': return S_ISSOCK(m);
        default:  return 0;
        }
    }

    case PRED_EMPTY:
        if (S_ISREG(st->st_mode))  return st->st_size == 0;
        if (S_ISDIR(st->st_mode)) {
            DIR *d = opendir(path);
            if (!d) return 0;
            struct dirent *ent;
            int empty = 1;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                    empty = 0;
                    break;
                }
            }
            closedir(d);
            return empty;
        }
        return 0;

    case PRED_NEWER:
        return st->st_mtime > n->newer_mtime;

    case PRED_MTIME: {
        time_t now = time(NULL);
        long days = (long)((now - st->st_mtime) / 86400);
        return cmp_matches(n->time_cmp, days);
    }

    case PRED_ATIME: {
        time_t now = time(NULL);
        long days = (long)((now - st->st_atime) / 86400);
        return cmp_matches(n->time_cmp, days);
    }

    case PRED_SIZE: {
        long sz;
        switch (n->size_unit) {
        case SZ_BYTES:  sz = (long)st->st_size;                    break;
        case SZ_KILO:   sz = (long)((st->st_size + 1023) / 1024);  break;
        case SZ_MEGA:   sz = (long)((st->st_size + (1<<20)-1) >> 20); break;
        case SZ_GIGA:   sz = (long)((st->st_size + (1LL<<30)-1) >> 30); break;
        case SZ_BLOCKS: sz = (long)((st->st_size + 511) / 512);    break;
        default:        sz = (long)st->st_size; break;
        }
        return cmp_matches(n->size_cmp, sz);
    }

    case PRED_PERM:
        if (n->perm_exact)
            return (st->st_mode & 07777) == n->perm_mode;
        return (st->st_mode & n->perm_mode) == n->perm_mode;

    case PRED_USER:
        return st->st_uid == n->uid;

    case PRED_GROUP:
        return st->st_gid == n->gid;

    case PRED_PRINT:
        printf("%s\n", path);
        return 1;

    case PRED_PRINT0:
        fputs(path, stdout);
        putchar('\0');
        return 1;

    case PRED_DELETE: {
        int r;
        if (S_ISDIR(st->st_mode))
            r = rmdir(path);
        else
            r = unlink(path);
        if (r != 0)
            err_sys("find", "cannot delete '%s'", path);
        return r == 0;
    }

    case PRED_EXEC:
        return exec_cmd_single(n, path);

    case PRED_EXEC_PLUS:
        /* Accumulation handled at traversal level; eval as single here */
        return exec_cmd_single(n, path);

    default:
        return 1;
    }
    (void)depth;
}

/* ------------------------------------------------------------------ */
/* Traversal                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    expr_node_t *expr;
    find_args_t *fa;
    int          ret;
} walk_ctx_t;

static void walk_dir(const char *path, int depth, walk_ctx_t *ctx);

static void process_entry(const char *path, const struct stat *st,
                          int depth, walk_ctx_t *ctx)
{
    find_args_t *fa = ctx->fa;

    if (depth < fa->mindepth)
        goto recurse;

    if (fa->maxdepth >= 0 && depth > fa->maxdepth)
        return;

    const char *base = path_basename(path);
    int matched = eval_expr(ctx->expr, path, base, st, depth);

    /* If no explicit action was specified and we matched, default print */
    if (matched && !fa->has_action)
        printf("%s\n", path);

recurse:
    if (S_ISDIR(st->st_mode)) {
        if (fa->maxdepth < 0 || depth < fa->maxdepth)
            walk_dir(path, depth + 1, ctx);
    }
}

static void walk_dir(const char *path, int depth, walk_ctx_t *ctx)
{
    find_args_t *fa = ctx->fa;

    DIR *dir = opendir(path);
    if (!dir) {
        err_sys("find", "'%s'", path);
        ctx->ret = 1;
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        if (!path_join(path, ent->d_name, child)) {
            err_msg("find", "path too long: %s/%s", path, ent->d_name);
            ctx->ret = 1;
            continue;
        }

        struct stat st;
        int r;
        if (fa->follow_symlinks)
            r = stat(child, &st);
        else
            r = lstat(child, &st);

        if (r != 0) {
            err_sys("find", "'%s'", child);
            ctx->ret = 1;
            continue;
        }

        process_entry(child, &st, depth, ctx);
    }

    closedir(dir);
}

/* ------------------------------------------------------------------ */
/* Main applet entry                                                  */
/* ------------------------------------------------------------------ */

int applet_find(int argc, char **argv)
{
    find_args_t fa;
    memset(&fa, 0, sizeof(fa));
    fa.maxdepth = -1; /* unlimited */
    fa.mindepth = 0;

    /* Collect start paths and locate start of expression */
    int i = 1;
    int n_starts = 0;
    char *starts[256];

    /* Paths come before any expression primaries (which start with '-', '(', '!') */
    while (i < argc) {
        const char *a = argv[i];
        if (a[0] == '-' || strcmp(a, "(") == 0 || strcmp(a, "!") == 0)
            break;
        if (n_starts < 256)
            starts[n_starts++] = argv[i];
        i++;
    }

    /* Handle -L/-H/-P symlink flags that may appear before paths */
    /* Re-scan argv[1..] for -L etc. before we set up fa */
    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-L") == 0) { fa.follow_symlinks = 1; }
        else if (strcmp(argv[k], "-H") == 0) { fa.follow_symlinks = 0; }
        else if (strcmp(argv[k], "-P") == 0) { fa.follow_symlinks = 0; }
        else break;
    }

    /* Default start: current directory */
    if (n_starts == 0) {
        starts[0] = (char *)".";
        n_starts = 1;
    }

    /* Parse expression from remaining argv */
    fa.argv = argv;
    fa.argc = argc;
    fa.pos  = i;

    expr_node_t *expr = NULL;
    if (fa.pos < fa.argc) {
        expr = parse_expr(&fa);
        if (!expr) {
            return 1;
        }
    }

    /* Add default -print action if no action specified */
    /* (handled in process_entry) */

    walk_ctx_t ctx;
    ctx.expr = expr;
    ctx.fa   = &fa;
    ctx.ret  = 0;

    for (int j = 0; j < n_starts; j++) {
        const char *start = starts[j];
        struct stat st;
        int r = fa.follow_symlinks ? stat(start, &st) : lstat(start, &st);
        if (r != 0) {
            err_sys("find", "'%s'", start);
            ctx.ret = 1;
            continue;
        }
        /* Process the start point itself at depth 0 */
        process_entry(start, &st, 0, &ctx);
    }

    node_free(expr);
    return ctx.ret;
}
