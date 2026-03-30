/* xargs.c — xargs builtin: build and execute command lines from stdin */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/strbuf.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

#define DEFAULT_MAX_ARGS  5000
#define MAX_PARALLEL      512

typedef struct {
    int    opt_0;       /* -0: NUL delimiter */
    int    opt_r;       /* -r: no-run-if-empty */
    int    opt_n;       /* -n MAX: max args per invocation */
    int    opt_P;       /* -P MAX_PROCS: parallel */
    char  *opt_I;       /* -I REPL: replace string */
    char   opt_d;       /* -d DELIM: custom delimiter */
    int    has_d;       /* -d was specified */
    long   arg_max;     /* sysconf(_SC_ARG_MAX) - headroom */
    const char *opt_a;  /* -a FILE: read args from file instead of stdin */
    int    opt_L;       /* -L N: max lines per invocation */
    long   opt_s;       /* -s N: max command-line bytes (overrides arg_max) */
} xargs_opts_t;

/* ------------------------------------------------------------------ */
/* Token reading                                                      */
/* ------------------------------------------------------------------ */

/*
 * Read one token from stdin according to the delimiter mode.
 * Returns 1 if a token was read (into sb), 0 on EOF, -1 on error.
 *
 * Modes:
 *   -0:   split on NUL only, no quoting
 *   -d:   split on opts->opt_d, no quoting
 *   default: split on whitespace, respect single/double quoting
 */
static int read_token(strbuf_t *sb, const xargs_opts_t *opts)
{
    sb_reset(sb);
    int c;

    if (opts->opt_0) {
        /* NUL-delimited: read until NUL or EOF */
        for (;;) {
            c = getchar();
            if (c == EOF)
                return sb_len(sb) > 0 ? 1 : 0;
            if (c == '\0')
                return 1; /* token complete (may be empty string) */
            if (sb_appendc(sb, (char)c) != 0) {
                err_msg("xargs", "out of memory");
                return -1;
            }
        }
    }

    if (opts->has_d) {
        /* Custom delimiter: no quoting */
        for (;;) {
            c = getchar();
            if (c == EOF)
                return sb_len(sb) > 0 ? 1 : 0;
            if (c == (unsigned char)opts->opt_d)
                return 1;
            if (sb_appendc(sb, (char)c) != 0) {
                err_msg("xargs", "out of memory");
                return -1;
            }
        }
    }

    /* Default: whitespace-delimited with shell-style quoting */

    /* Skip leading whitespace */
    for (;;) {
        c = getchar();
        if (c == EOF) return 0;
        if (!isspace((unsigned char)c)) break;
    }

    /* Read token with quoting */
    int in_single = 0, in_double = 0;
    for (;;) {
        if (c == EOF) {
            if (in_single || in_double) {
                err_msg("xargs", "unmatched quote");
                return -1;
            }
            return sb_len(sb) > 0 ? 1 : 0;
        }

        if (in_single) {
            if (c == '\'') {
                in_single = 0;
            } else {
                if (sb_appendc(sb, (char)c) != 0) {
                    err_msg("xargs", "out of memory");
                    return -1;
                }
            }
        } else if (in_double) {
            if (c == '"') {
                in_double = 0;
            } else if (c == '\\') {
                int next = getchar();
                if (next == EOF) {
                    err_msg("xargs", "unexpected EOF after backslash");
                    return -1;
                }
                /* Only a few escapes are meaningful inside double quotes */
                if (next == '"' || next == '\\' || next == '$' || next == '`') {
                    if (sb_appendc(sb, (char)next) != 0) {
                        err_msg("xargs", "out of memory");
                        return -1;
                    }
                } else {
                    if (sb_appendc(sb, '\\')     != 0 ||
                        sb_appendc(sb, (char)next) != 0) {
                        err_msg("xargs", "out of memory");
                        return -1;
                    }
                }
            } else {
                if (sb_appendc(sb, (char)c) != 0) {
                    err_msg("xargs", "out of memory");
                    return -1;
                }
            }
        } else {
            /* Unquoted */
            if (isspace((unsigned char)c))
                return sb_len(sb) > 0 ? 1 : 0; /* token boundary */

            if (c == '\'') {
                in_single = 1;
            } else if (c == '"') {
                in_double = 1;
            } else if (c == '\\') {
                int next = getchar();
                if (next == EOF) {
                    err_msg("xargs", "unexpected EOF after backslash");
                    return -1;
                }
                if (sb_appendc(sb, (char)next) != 0) {
                    err_msg("xargs", "out of memory");
                    return -1;
                }
            } else {
                if (sb_appendc(sb, (char)c) != 0) {
                    err_msg("xargs", "out of memory");
                    return -1;
                }
            }
        }

        c = getchar();
    }
}

/* ------------------------------------------------------------------ */
/* Subprocess execution                                               */
/* ------------------------------------------------------------------ */

/*
 * Fork and exec av[0] with the given argv.
 * Returns the child's exit status (0 = success).
 */
static int exec_cmd(char **av, int ac)
{
    (void)ac;
    pid_t pid = fork();
    if (pid < 0) {
        err_sys("xargs", "fork");
        return 1;
    }
    if (pid == 0) {
        execvp(av[0], av);
        /* execvp failed */
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        err_sys("xargs", "waitpid");
        return 1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Parallel execution support                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    pid_t pid;
    int   in_use;
} proc_slot_t;

static proc_slot_t g_procs[MAX_PARALLEL];
static int         g_nprocs = 0;

/* Wait for one child to finish; update slot table. */
static int wait_one(void)
{
    int   status;
    pid_t pid = waitpid(-1, &status, 0);
    if (pid < 0) return 0;

    for (int i = 0; i < g_nprocs; i++) {
        if (g_procs[i].pid == pid) {
            g_procs[i].in_use = 0;
            break;
        }
    }

    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int active_procs(void)
{
    int n = 0;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i].in_use) n++;
    return n;
}

/*
 * Fork a process for av, managing up to max_procs concurrent children.
 * If max_procs == 1, falls back to synchronous exec_cmd().
 * Returns 0 on fork success, error code if exec_cmd was used.
 */
static int exec_cmd_parallel(char **av, int ac, int max_procs)
{
    if (max_procs <= 1)
        return exec_cmd(av, ac);

    /* Block until a slot is free */
    while (active_procs() >= max_procs)
        wait_one();

    pid_t pid = fork();
    if (pid < 0) {
        err_sys("xargs", "fork");
        return 1;
    }
    if (pid == 0) {
        execvp(av[0], av);
        _exit(errno == ENOENT ? 127 : 126);
    }

    /* Record in slot table */
    for (int i = 0; i < g_nprocs; i++) {
        if (!g_procs[i].in_use) {
            g_procs[i].pid    = pid;
            g_procs[i].in_use = 1;
            return 0;
        }
    }
    if (g_nprocs < MAX_PARALLEL) {
        g_procs[g_nprocs].pid    = pid;
        g_procs[g_nprocs].in_use = 1;
        g_nprocs++;
    }
    return 0;
}

/* Wait for all remaining parallel children; return worst exit status. */
static int wait_all_children(void)
{
    int worst = 0;
    while (active_procs() > 0) {
        int r = wait_one();
        if (r > worst) worst = r;
    }
    return worst;
}

/* ------------------------------------------------------------------ */
/* -I REPL replacement                                               */
/* ------------------------------------------------------------------ */

/*
 * Replace all occurrences of repl in tmpl with item.
 * Returns heap-allocated result string, or NULL on OOM.
 */
static char *replace_str(const char *tmpl, const char *repl, const char *item)
{
    strbuf_t sb;
    if (sb_init(&sb, 64) != 0) return NULL;

    size_t      rlen = strlen(repl);
    const char *p    = tmpl;

    for (;;) {
        const char *found = strstr(p, repl);
        if (!found) {
            if (sb_append(&sb, p) != 0) { sb_free(&sb); return NULL; }
            break;
        }
        if (sb_appendn(&sb, p, (size_t)(found - p)) != 0 ||
            sb_append(&sb, item) != 0) {
            sb_free(&sb);
            return NULL;
        }
        p = found + rlen;
    }

    char *result = strdup(sb_str(&sb));
    sb_free(&sb);
    return result;
}

/* ------------------------------------------------------------------ */
/* Flush accumulated items: build argv and exec                       */
/* ------------------------------------------------------------------ */

static int flush_items(char **cmd_argv, int cmd_argc,
                       char **items, int n_items,
                       char **run_av, int av_cap,
                       int max_procs)
{
    int ac = cmd_argc + n_items;
    if (ac + 1 > av_cap) {
        /* Caller must ensure capacity; this should not happen */
        err_msg("xargs", "internal: argv overflow");
        return 1;
    }
    for (int k = 0; k < cmd_argc; k++)
        run_av[k] = cmd_argv[k];
    for (int k = 0; k < n_items; k++)
        run_av[cmd_argc + k] = items[k];
    run_av[ac] = NULL;

    return exec_cmd_parallel(run_av, ac, max_procs);
}

/* ------------------------------------------------------------------ */
/* Main applet entry                                                  */
/* ------------------------------------------------------------------ */

int applet_xargs(int argc, char **argv)
{
    xargs_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.opt_n = DEFAULT_MAX_ARGS;
    opts.opt_P = 1;

    long sc_arg_max = sysconf(_SC_ARG_MAX);
    if (sc_arg_max <= 0)
        sc_arg_max = 131072;
    opts.arg_max = sc_arg_max - 2048;
    if (opts.arg_max < 4096)
        opts.arg_max = 4096;

    int i;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strcmp(arg, "-0") == 0) { opts.opt_0 = 1; continue; }

        if (strcmp(arg, "-r") == 0 ||
            strcmp(arg, "--no-run-if-empty") == 0) {
            opts.opt_r = 1; continue;
        }

        if (strcmp(arg, "-n") == 0) {
            if (++i >= argc) { err_msg("xargs", "-n requires argument"); return 1; }
            opts.opt_n = (int)strtol(argv[i], NULL, 10);
            if (opts.opt_n < 1) { err_msg("xargs", "-n must be >= 1"); return 1; }
            continue;
        }
        if (strncmp(arg, "-n", 2) == 0 && arg[2]) {
            opts.opt_n = (int)strtol(arg + 2, NULL, 10);
            if (opts.opt_n < 1) { err_msg("xargs", "-n must be >= 1"); return 1; }
            continue;
        }

        if (strcmp(arg, "-P") == 0) {
            if (++i >= argc) { err_msg("xargs", "-P requires argument"); return 1; }
            opts.opt_P = (int)strtol(argv[i], NULL, 10);
            if (opts.opt_P < 1)           opts.opt_P = 1;
            if (opts.opt_P > MAX_PARALLEL) opts.opt_P = MAX_PARALLEL;
            continue;
        }
        if (strncmp(arg, "-P", 2) == 0 && arg[2]) {
            opts.opt_P = (int)strtol(arg + 2, NULL, 10);
            if (opts.opt_P < 1)           opts.opt_P = 1;
            if (opts.opt_P > MAX_PARALLEL) opts.opt_P = MAX_PARALLEL;
            continue;
        }

        if (strcmp(arg, "-I") == 0) {
            if (++i >= argc) { err_msg("xargs", "-I requires argument"); return 1; }
            opts.opt_I = argv[i];
            opts.opt_n = 1; /* -I implies -n 1 */
            continue;
        }
        if (strncmp(arg, "-I", 2) == 0 && arg[2]) {
            opts.opt_I = (char *)(arg + 2);
            opts.opt_n = 1;
            continue;
        }

        if (strcmp(arg, "-d") == 0) {
            if (++i >= argc) { err_msg("xargs", "-d requires argument"); return 1; }
            opts.opt_d = argv[i][0];
            opts.has_d = 1;
            continue;
        }
        if (strncmp(arg, "-d", 2) == 0 && arg[2]) {
            opts.opt_d = arg[2];
            opts.has_d = 1;
            continue;
        }

        if (strcmp(arg, "-a") == 0) {
            if (++i >= argc) { err_msg("xargs", "-a requires argument"); return 1; }
            opts.opt_a = argv[i];
            continue;
        }
        if (strncmp(arg, "-a", 2) == 0 && arg[2]) {
            opts.opt_a = arg + 2;
            continue;
        }

        if (strcmp(arg, "-L") == 0) {
            if (++i >= argc) { err_msg("xargs", "-L requires argument"); return 1; }
            opts.opt_L = (int)strtol(argv[i], NULL, 10);
            if (opts.opt_L < 1) { err_msg("xargs", "-L must be >= 1"); return 1; }
            continue;
        }
        if (strncmp(arg, "-L", 2) == 0 && arg[2]) {
            opts.opt_L = (int)strtol(arg + 2, NULL, 10);
            if (opts.opt_L < 1) { err_msg("xargs", "-L must be >= 1"); return 1; }
            continue;
        }

        if (strcmp(arg, "-s") == 0) {
            if (++i >= argc) { err_msg("xargs", "-s requires argument"); return 1; }
            opts.opt_s = strtol(argv[i], NULL, 10);
            continue;
        }
        if (strncmp(arg, "-s", 2) == 0 && arg[2]) {
            opts.opt_s = strtol(arg + 2, NULL, 10);
            continue;
        }

        if (arg[0] == '-') {
            err_msg("xargs", "unrecognized option '%s'", arg);
            return 1;
        }

        break; /* rest is command + args */
    }

    /* -a FILE: redirect stdin from file */
    if (opts.opt_a) {
        int fd = open(opts.opt_a, O_RDONLY);
        if (fd < 0) {
            err_msg("xargs", "cannot open '%s'", opts.opt_a);
            return 1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            err_msg("xargs", "dup2 failed");
            close(fd);
            return 1;
        }
        close(fd);
    }

    /* -s N: override arg_max */
    if (opts.opt_s > 0 && opts.opt_s < opts.arg_max)
        opts.arg_max = opts.opt_s;

    /* -L N: treat like -n N (max items per invocation; simple impl) */
    if (opts.opt_L > 0 && opts.opt_L < opts.opt_n)
        opts.opt_n = opts.opt_L;

    /* Command template from remaining argv */
    int    cmd_argc;
    char **cmd_argv;
    int    cmd_allocated = 0;

    if (i >= argc) {
        /* Default command: /bin/echo */
        cmd_argv = malloc(2 * sizeof(char *));
        if (!cmd_argv) { err_msg("xargs", "out of memory"); return 1; }
        cmd_argv[0] = (char *)"/bin/echo";
        cmd_argv[1] = NULL;
        cmd_argc    = 1;
        cmd_allocated = 1;
    } else {
        cmd_argc = argc - i;
        cmd_argv = malloc((size_t)(cmd_argc + 1) * sizeof(char *));
        if (!cmd_argv) { err_msg("xargs", "out of memory"); return 1; }
        for (int k = 0; k < cmd_argc; k++)
            cmd_argv[k] = argv[i + k];
        cmd_argv[cmd_argc] = NULL;
        cmd_allocated = 1;
    }

    /* Pre-compute fixed part of command line length */
    long cmd_fixed_len = 0;
    for (int k = 0; k < cmd_argc; k++)
        cmd_fixed_len += (long)strlen(cmd_argv[k]) + 1;

    /* Working argv for non -I invocations */
    int    av_cap = cmd_argc + opts.opt_n + 2;
    char **run_av = malloc((size_t)av_cap * sizeof(char *));
    if (!run_av) {
        err_msg("xargs", "out of memory");
        free(cmd_argv);
        return 1;
    }

    /* Token buffer */
    strbuf_t tok;
    if (sb_init(&tok, 256) != 0) {
        err_msg("xargs", "out of memory");
        free(run_av);
        free(cmd_argv);
        return 1;
    }

    /* Accumulated item list */
    int    items_cap = opts.opt_n + 4;
    char **items     = malloc((size_t)items_cap * sizeof(char *));
    if (!items) {
        err_msg("xargs", "out of memory");
        sb_free(&tok);
        free(run_av);
        free(cmd_argv);
        return 1;
    }

    int    n_items   = 0;
    long   items_len = 0; /* total length of accumulated item strings */
    int    worst     = 0;
    int    got_input = 0;

    /* ---------------------------------------------------------------- */
    /* Main token loop                                                  */
    /* ---------------------------------------------------------------- */

    for (;;) {
        int r = read_token(&tok, &opts);
        if (r < 0) {
            worst = 1;
            break;
        }
        if (r == 0)
            break; /* EOF */

        got_input = 1;
        const char *token = sb_str(&tok);

        /* -I REPL mode: execute once per item with substitution */
        if (opts.opt_I) {
            int sub_ac = cmd_argc;
            char **sub_av = malloc((size_t)(sub_ac + 1) * sizeof(char *));
            if (!sub_av) {
                err_msg("xargs", "out of memory");
                worst = 1;
                break;
            }

            int ok = 1;
            for (int k = 0; k < cmd_argc; k++) {
                sub_av[k] = replace_str(cmd_argv[k], opts.opt_I, token);
                if (!sub_av[k]) {
                    err_msg("xargs", "out of memory");
                    for (int m = 0; m < k; m++) free(sub_av[m]);
                    free(sub_av);
                    ok = 0;
                    worst = 1;
                    break;
                }
            }
            if (!ok) break;
            sub_av[sub_ac] = NULL;

            int rc = exec_cmd_parallel(sub_av, sub_ac, opts.opt_P);
            if (rc > worst) worst = rc;

            for (int k = 0; k < sub_ac; k++) free(sub_av[k]);
            free(sub_av);
            continue;
        }

        /* Normal accumulation mode */
        long token_len = (long)strlen(token) + 1;

        /* Flush if -n limit or arg_max would be exceeded */
        if (n_items > 0 &&
            (n_items >= opts.opt_n ||
             cmd_fixed_len + items_len + token_len > opts.arg_max)) {

            /* Grow run_av if needed */
            int need_cap = cmd_argc + n_items + 1;
            if (need_cap > av_cap) {
                av_cap = need_cap + 4;
                char **tmp = realloc(run_av, (size_t)av_cap * sizeof(char *));
                if (!tmp) {
                    err_msg("xargs", "out of memory");
                    worst = 1;
                    goto done;
                }
                run_av = tmp;
            }

            int rc = flush_items(cmd_argv, cmd_argc, items, n_items,
                                 run_av, av_cap, opts.opt_P);
            if (rc > worst) worst = rc;

            for (int k = 0; k < n_items; k++) free(items[k]);
            n_items   = 0;
            items_len = 0;
        }

        /* Grow items array if needed */
        if (n_items >= items_cap - 1) {
            items_cap = items_cap * 2 + 4;
            char **tmp = realloc(items, (size_t)items_cap * sizeof(char *));
            if (!tmp) {
                err_msg("xargs", "out of memory");
                worst = 1;
                goto done;
            }
            items = tmp;
        }

        items[n_items] = strdup(token);
        if (!items[n_items]) {
            err_msg("xargs", "out of memory");
            worst = 1;
            goto done;
        }
        n_items++;
        items_len += token_len;
    }

    /* Flush any remaining items */
    if (n_items > 0) {
        int need_cap = cmd_argc + n_items + 1;
        if (need_cap > av_cap) {
            av_cap = need_cap + 4;
            char **tmp = realloc(run_av, (size_t)av_cap * sizeof(char *));
            if (!tmp) {
                err_msg("xargs", "out of memory");
                worst = 1;
                goto done;
            }
            run_av = tmp;
        }
        int rc = flush_items(cmd_argv, cmd_argc, items, n_items,
                             run_av, av_cap, opts.opt_P);
        if (rc > worst) worst = rc;

        for (int k = 0; k < n_items; k++) free(items[k]);
        n_items = 0;
    } else if (!got_input && !opts.opt_r && !opts.opt_I) {
        /*
         * -r not set and no input received: run command with no appended args
         * (POSIX behaviour when input is empty).
         */
        for (int k = 0; k < cmd_argc; k++)
            run_av[k] = cmd_argv[k];
        run_av[cmd_argc] = NULL;
        int rc = exec_cmd_parallel(run_av, cmd_argc, opts.opt_P);
        if (rc > worst) worst = rc;
    }

done:
    {
        int r = wait_all_children();
        if (r > worst) worst = r;
    }

    sb_free(&tok);
    free(items);
    free(run_av);
    if (cmd_allocated) free(cmd_argv);

    /* Exit codes: 0 = success, 1-125 = child error, 126/127 = exec error */
    if (worst > 125) return 1;
    return worst;
}
