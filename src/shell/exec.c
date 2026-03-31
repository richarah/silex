/* exec.c — shell command execution: builtins, pipelines, and fork/exec */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "exec.h"
#include "expand.h"
#include "redirect.h"
#include "shell.h"
#include "../applets.h"
#include "../cache/fscache.h"
#include "../util/arena.h"
#include "../util/section.h"
#include "../util/strbuf.h"

#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

static int exec_builtin_set(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_export(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_unset(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_readonly(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_cd(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_shift(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_exit(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_true(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_false(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_colon(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_eval(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_exec_cmd(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_trap(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_read(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_printf_sh(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_test(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_local(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_return(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_wait(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_source(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_break(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_continue(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_umask(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_command(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_type(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_getopts(shell_ctx_t *sh, int argc, char **argv);

/* -------------------------------------------------------------------------
 * Special exit codes for break/continue/return flow control
 * These are interpreted only within exec_node, not propagated to caller.
 * ------------------------------------------------------------------------- */

#define FLOW_BREAK    200
#define FLOW_CONTINUE 201
#define FLOW_RETURN   202

/* -------------------------------------------------------------------------
 * Applet lookup (delegates to global find_applet() from main.c / applets.h)
 * This enables in-process builtin short-circuit for all 27 applets.
 * ------------------------------------------------------------------------- */

static const applet_t *find_applet_by_name(const char *name)
{
    return find_applet(name);
}

/* -------------------------------------------------------------------------
 * Shell-internal builtins table
 * ------------------------------------------------------------------------- */

typedef int (*shell_builtin_fn)(shell_ctx_t *, int, char **);

typedef struct {
    const char      *name;
    shell_builtin_fn fn;
} shell_builtin_t;

static const shell_builtin_t shell_builtins[] = {
    { ":",        exec_builtin_colon     },
    { "true",     exec_builtin_true      },
    { "false",    exec_builtin_false     },
    { "set",      exec_builtin_set       },
    { "export",   exec_builtin_export    },
    { "unset",    exec_builtin_unset     },
    { "readonly", exec_builtin_readonly  },
    { "cd",       exec_builtin_cd        },
    { "shift",    exec_builtin_shift     },
    { "exit",     exec_builtin_exit      },
    { "eval",     exec_builtin_eval      },
    { "exec",     exec_builtin_exec_cmd  },
    { "trap",     exec_builtin_trap      },
    { "read",     exec_builtin_read      },
    { "printf",   exec_builtin_printf_sh },
    { "test",     exec_builtin_test      },
    { "[",        exec_builtin_test      },
    { "local",    exec_builtin_local     },
    { "return",   exec_builtin_return    },
    { "wait",     exec_builtin_wait      },
    { ".",        exec_builtin_source    },
    { "source",   exec_builtin_source    },
    { "break",    exec_builtin_break     },
    { "continue", exec_builtin_continue  },
    { "umask",    exec_builtin_umask     },
    { "command",  exec_builtin_command   },
    { "type",     exec_builtin_type      },
    { "getopts",  exec_builtin_getopts   },
    { NULL, NULL }
};

static shell_builtin_fn find_shell_builtin(const char *name)
{
    for (const shell_builtin_t *b = shell_builtins; b->name; b++) {
        if (strcmp(b->name, name) == 0)
            return b->fn;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Function registry helpers
 * Store function definitions as (name, node_t*) pairs.
 * sh->funcs[256] is used as a simple open-addressing hash.
 * ------------------------------------------------------------------------- */

typedef struct func_entry {
    char        *name;
    node_t      *body;
    struct func_entry *next;
} func_entry_t;

static unsigned int func_hash(const char *s)
{
    unsigned int h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h & 255u;
}

static void func_register(shell_ctx_t *sh, const char *name, node_t *body)
{
    unsigned int idx = func_hash(name);
    func_entry_t *e  = sh->funcs[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) {
            e->body = body;
            return;
        }
        e = e->next;
    }
    /* Allocate new entry in arena */
    func_entry_t *ne = arena_alloc(&sh->parse_arena, sizeof(func_entry_t));
    ne->name  = arena_strdup(&sh->parse_arena, name);
    ne->body  = body;
    ne->next  = sh->funcs[idx];
    sh->funcs[idx] = ne;
}

static node_t *func_lookup(shell_ctx_t *sh, const char *name)
{
    unsigned int idx = func_hash(name);
    func_entry_t *e  = sh->funcs[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) return e->body;
        e = e->next;
    }
    return NULL;
}

static void func_unregister(shell_ctx_t *sh, const char *name)
{
    unsigned int idx = func_hash(name);
    func_entry_t **pp = (func_entry_t **)&sh->funcs[idx];
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            *pp = (*pp)->next;   /* unlink (arena-allocated; not freed) */
            return;
        }
        pp = &(*pp)->next;
    }
}

/* -------------------------------------------------------------------------
 * PATH lookup cache (F-03)
 * Maps command name → resolved absolute path.
 * Invalidated automatically when PATH changes (FNV-1a hash comparison).
 * Entries are malloc'd; freed by path_cache_clear() / shell_free().
 * ------------------------------------------------------------------------- */

typedef struct path_cache_entry {
    char *name;
    char *path;   /* malloc'd absolute path, or NULL if not found */
    int   found;
    struct path_cache_entry *next;
} path_cache_entry_t;

static uint32_t fnv1a_str(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

void path_cache_clear(shell_ctx_t *sh)
{
    for (int i = 0; i < 256; i++) {
        path_cache_entry_t *e = sh->path_cache[i];
        while (e) {
            path_cache_entry_t *nxt = e->next;
            free(e->name);
            free(e->path);
            free(e);
            e = nxt;
        }
        sh->path_cache[i] = NULL;
    }
    sh->path_cache_hash = 0;
}

/* Validate cache against current PATH, then look up name.
 * Returns: pointer to entry if cached, NULL if not yet cached. */
static path_cache_entry_t *path_cache_get(shell_ctx_t *sh, const char *name)
{
    const char *pathval = vars_get(&sh->vars, "PATH");
    if (!pathval) pathval = "";
    uint32_t h = fnv1a_str(pathval);
    if (h != sh->path_cache_hash) {
        path_cache_clear(sh);
        sh->path_cache_hash = h;
    }
    unsigned int idx = fnv1a_str(name) & 255u;
    for (path_cache_entry_t *e = sh->path_cache[idx]; e; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static void path_cache_put(shell_ctx_t *sh, const char *name,
                           const char *resolved)
{
    unsigned int idx = fnv1a_str(name) & 255u;
    path_cache_entry_t *e = malloc(sizeof(*e));
    if (unlikely(!e)) return;
    e->name  = strdup(name);
    e->path  = resolved ? strdup(resolved) : NULL;
    e->found = resolved ? 1 : 0;
    e->next  = sh->path_cache[idx];
    if (unlikely(!e->name || (resolved && !e->path))) { free(e->name); free(e->path); free(e); return; }
    sh->path_cache[idx] = e;
}

/* Search PATH dirs for an executable named `name`.
 * Writes the full path into buf[bufsz]. Returns buf on success, NULL if not found. */
static char *path_resolve(shell_ctx_t *sh, const char *name,
                          char *buf, size_t bufsz)
{
    if (strchr(name, '/')) {
        /* Absolute or relative path: check directly */
        struct stat st;
        if (stat(name, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            if (strlen(name) < bufsz) {
                strcpy(buf, name);
                return buf;
            }
        }
        return NULL;
    }

    const char *pathval = vars_get(&sh->vars, "PATH");
    if (!pathval || !*pathval) return NULL;

    char *pathcopy = strdup(pathval);
    if (!pathcopy) return NULL;

    char *saveptr = NULL;
    char *dir = strtok_r(pathcopy, ":", &saveptr);
    char *result = NULL;
    while (dir) {
        int n = snprintf(buf, bufsz, "%s/%s", dir, name);
        if (n > 0 && (size_t)n < bufsz) {
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) &&
                (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
                result = buf;
                break;
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(pathcopy);
    return result;
}

/* -------------------------------------------------------------------------
 * exec_simple_cmd
 * ------------------------------------------------------------------------- */

int exec_simple_cmd(shell_ctx_t *sh, char **words, char **assigns, redir_t *redirs)
{
    /* 1. Expand + apply variable assignments.
     * When no command follows, set in shell scope (normal assignment).
     * When a command follows, set in environment only (env-prefix), cleaned up after. */
    int assign_err = 0;
    int nassigns = 0;
    if (assigns) while (assigns[nassigns]) nassigns++;

    /* Expand all assign values up front.
     * Track cmd-sub exit status: per POSIX, assignment-only commands exit with
     * the status of the last command substitution (or 0 if none). */
    char **anames = NULL, **avals = NULL;
    int cmdsub_exit = 0;
    if (nassigns > 0) {
        anames = malloc((size_t)nassigns * sizeof(char *));
        avals  = malloc((size_t)nassigns * sizeof(char *));
        int prev_exit = sh->last_exit;
        sh->last_exit = 0;  /* default: 0 unless cmd-sub sets it */
        for (int i = 0; i < nassigns; i++) {
            const char *eq = strchr(assigns[i], '=');
            if (!eq) { anames[i] = NULL; avals[i] = NULL; continue; }
            size_t nlen = (size_t)(eq - assigns[i]);
            anames[i] = strndup(assigns[i], nlen);
            avals[i]  = expand_word(sh, eq + 1);
        }
        cmdsub_exit = sh->last_exit;  /* 0 or last cmd-sub exit */
        sh->last_exit = prev_exit;    /* restore for with-command path */
    }

    /* If no command words, apply to shell scope */
    if (unlikely(!words || !words[0])) {
        for (int i = 0; i < nassigns; i++) {
            if (!anames[i]) continue;
            if (vars_set(&sh->vars, anames[i], avals[i] ? avals[i] : "") != 0)
                assign_err = 1;
            free(anames[i]);
        }
        free(anames); free(avals);
        if (redirs) {
            redirect_ctx_t rctx = {NULL, 0};
            redirect_apply(sh, redirs, &rctx);
            redirect_restore(&rctx);
        }
        /* Return cmd-sub exit status (POSIX: last cmd-sub exit, or 0 if none) */
        return assign_err ? 1 : cmdsub_exit;
    }

    /* With command: set env temporarily (will be cleaned up after command) */
    for (int i = 0; i < nassigns; i++) {
        if (anames[i])
            setenv(anames[i], avals[i] ? avals[i] : "", 1);
    }

    /* 2. Expand all words */
    char **expanded = expand_words(sh, words);
    int cmd_rc = 0;
    if (unlikely(!expanded || !expanded[0]))
        goto cmd_done;

    {
    const char *cmd = expanded[0];

    /* Count argc */
    int argc = 0;
    while (expanded[argc]) argc++;

    /* SILEX_TRACE: print command before execution */
    if (sh->trace_level >= 1) {
        fputs("+ ", stderr);
        for (int ti = 0; ti < argc; ti++) {
            if (ti) fputc(' ', stderr);
            fputs(expanded[ti], stderr);
        }
        fputc('\n', stderr);
    }

    /* 3. Check for shell-internal builtin (high priority — run in-process) */
    shell_builtin_fn sfn = find_shell_builtin(cmd);
    if (sfn) {
        if (sh->trace_level >= 2)
            fprintf(stderr, "+ [builtin] %s\n", cmd);
        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        if (redirs) redirect_apply(sh, redirs, &rctx);
        cmd_rc = sfn(sh, argc, expanded);
        /* 'exec' with no command: redirections are permanent (not restored) */
        if (strcmp(cmd, "exec") == 0 && argc == 1)
            redirect_commit(&rctx);
        else if (redirs)
            redirect_restore(&rctx);
        goto cmd_done;
    }

    /* 4. Check for shell function (user-defined functions override applets) */
    node_t *fnbody = func_lookup(sh, cmd);
    if (fnbody) {
        if (sh->call_depth >= SHELL_MAX_CALL_DEPTH) {
            fprintf(stderr, "silex: sh: %s: maximum call depth (%d) exceeded\n",
                    cmd, SHELL_MAX_CALL_DEPTH);
            cmd_rc = 1;
            goto cmd_done;
        }

        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        if (redirs) redirect_apply(sh, redirs, &rctx);

        /* Set positional parameters for function */
        char **old_pos = sh->positional;
        int old_n      = sh->positional_n;

        sh->positional_n = argc - 1;
        sh->positional   = expanded + 1;

        vars_push_scope(&sh->vars);
        sh->call_depth++;
        cmd_rc = exec_node(sh, fnbody);
        sh->call_depth--;
        vars_pop_scope(&sh->vars);

        sh->positional   = old_pos;
        sh->positional_n = old_n;

        if (redirs) redirect_restore(&rctx);

        /* Absorb FLOW_RETURN */
        if (cmd_rc == FLOW_RETURN) cmd_rc = sh->last_exit;
        goto cmd_done;
    }

    /* 5. Check for applet (run in-process, apply redirects) */
    const applet_t *ap = find_applet_by_name(cmd);
    if (ap) {
        /*
         * B-8: XC-02 Dead command elimination.
         * Skip `mkdir -p PATH...` when all PATH arguments are already
         * confirmed existing directories with written_by_silex=1
         * in fscache (i.e. we created them in this run).
         * Only safe when there are no redirections.
         */
        if (!redirs && strcmp(cmd, "mkdir") == 0 && argc >= 2) {
            int has_p = 0;
            for (int i = 1; i < argc; i++) {
                const char *a = expanded[i];
                if (!a) break;
                if (strcmp(a, "--") == 0) break;
                if (a[0] == '-') {
                    for (const char *f = a + 1; *f && *f != '-'; f++)
                        if (*f == 'p') { has_p = 1; break; }
                }
            }
            if (has_p) {
                int all_done = 1;
                for (int i = 1; i < argc && all_done; i++) {
                    const char *a = expanded[i];
                    if (!a) break;
                    if (a[0] == '-' || strcmp(a, "--") == 0) continue;
                    struct stat st;
                    if (!fscache_written_by_silex(a) ||
                        fscache_stat(a, &st) != 0 || !S_ISDIR(st.st_mode))
                        all_done = 0;
                }
                if (all_done) { cmd_rc = 0; goto cmd_done; }
            }
        }
        /* 'sh' applet may call exit(); run it in a fork so the parent survives.
         * Also exports vars so they are visible to the sub-shell. */
        if (strcmp(cmd, "sh") == 0) {
            fflush(NULL);
            pid_t apid = fork();
            if (apid < 0) { perror("fork"); cmd_rc = 1; goto cmd_done; }
            if (apid == 0) {
                redirect_ctx_t rctx2 = {NULL, 0};
                if (redirs) redirect_apply(sh, redirs, &rctx2);
                vars_export_env(&sh->vars);
                int aret = ap->fn(argc, expanded);
                fflush(NULL);
                _exit(aret);
            }
            int astatus;
            while (waitpid(apid, &astatus, 0) < 0 && errno == EINTR) {}
            if (WIFEXITED(astatus)) cmd_rc = WEXITSTATUS(astatus);
            else if (WIFSIGNALED(astatus)) cmd_rc = 128 + WTERMSIG(astatus);
            else cmd_rc = 1;
            goto cmd_done;
        }
        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        if (redirs) redirect_apply(sh, redirs, &rctx);
        cmd_rc = ap->fn(argc, expanded);
        if (redirs) redirect_restore(&rctx);
        goto cmd_done;
    }

    /* 6. External command: resolve PATH in parent, fork + execv */

    /* Resolve the command to an absolute path using the cache (F-03).
     * Commands containing '/' bypass the cache and are used as-is. */
    const char *exec_path;
    char path_buf[PATH_MAX];

    if (strchr(cmd, '/')) {
        exec_path = cmd;
    } else {
        path_cache_entry_t *ce = path_cache_get(sh, cmd);
        if (ce) {
            exec_path = ce->found ? ce->path : NULL;
        } else {
            char *rp = path_resolve(sh, cmd, path_buf, sizeof(path_buf));
            path_cache_put(sh, cmd, rp);
            exec_path = rp;  /* path_buf is valid for this scope */
        }
    }

    if (!exec_path) {
        fprintf(stderr, "silex: %s: command not found\n", cmd);
        sh->last_exit = 127;
        cmd_rc = 127;
        goto cmd_done;
    }

    pid_t pid = fork();
    if (unlikely(pid < 0)) {
        perror("fork");
        cmd_rc = 1;
        goto cmd_done;
    }

    if (pid == 0) {
        /* Child: restore SIGPIPE to default (shell set it to SIG_IGN) */
        signal(SIGPIPE, SIG_DFL);

        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        if (redirs) redirect_apply(sh, redirs, &rctx);

        /* Export all exported vars */
        vars_export_env(&sh->vars);

        execv(exec_path, expanded);
        perror(exec_path);
        _exit(127);
    }

    /* Parent */
    {
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status))
        cmd_rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        cmd_rc = 128 + WTERMSIG(status);
    else
        cmd_rc = 1;
    sh->last_exit = cmd_rc;
    /* B-1: external command may have changed filesystem state — invalidate all */
    fscache_invalidate_all();
    }
    } /* end command dispatch block */

cmd_done:
    /* Clean up env-prefix assigns: unsetenv + free */
    for (int i = 0; i < nassigns; i++) {
        if (anames[i]) {
            unsetenv(anames[i]);
            free(anames[i]);
        }
    }
    free(anames);
    free(avals);
    return cmd_rc;
}

/* -------------------------------------------------------------------------
 * exec_pipeline
 * ------------------------------------------------------------------------- */

/*
 * Pipe optimisation: eliminate useless `cat` (no args, no flags, no redirs)
 * at the head of a pipeline.
 *
 *   cat | APPLET args…     →  APPLET args…   (APPLET reads stdin directly)
 *
 * Only the literal word "cat" is matched; $VAR expansion is intentionally
 * skipped here to keep the fast path cheap.  Stages with file arguments or
 * flags are left unchanged because the semantics differ (multiple-file grep
 * adds filename prefixes, etc.).
 */
static void pipeline_elim_trivial_cat(node_t **stages, int *nstages)
{
    if (*nstages < 2) return;

    node_t *s0 = stages[0];
    if (s0->type != N_CMD) return;
    /* Must have no env-prefix assignments */
    if (s0->u.cmd.assigns && s0->u.cmd.assigns[0]) return;
    /* Must have no redirections */
    if (s0->u.cmd.redirs) return;
    /* Must have exactly one word: "cat" */
    if (!s0->u.cmd.words || !s0->u.cmd.words[0]) return;
    if (s0->u.cmd.words[1] != NULL) return;          /* has args/files */
    if (strcmp(s0->u.cmd.words[0], "cat") != 0) return;

    /* Safe: remove stage 0, shift remaining stages down */
    (*nstages)--;
    for (int i = 0; i < *nstages; i++)
        stages[i] = stages[i + 1];
}

int exec_pipeline(shell_ctx_t *sh, node_t *node)
{
    /* Collect pipeline stages into a flat array */
    node_t *stages[256];
    int     nstages = 0;

    node_t *cur = node;
    while (cur && cur->type == N_PIPE && nstages < 255) {
        stages[nstages++] = cur->u.binary.left;
        cur = cur->u.binary.right;
    }
    if (cur && nstages < 255)
        stages[nstages++] = cur;

    /* Pipe optimisation: eliminate trivial `cat | ...` */
    pipeline_elim_trivial_cat(stages, &nstages);

    if (nstages == 1)
        return exec_node(sh, stages[0]);

    /* Build pipes */
    int pipes[255][2];
    for (int i = 0; i < nstages - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return 1;
        }
    }

    pid_t pids[256];

    for (int i = 0; i < nstages; i++) {
        /* Last stage may be a builtin — run in-process to allow pipefail */
        int is_last = (i == nstages - 1);

        /* For builtins as last stage: run in current process but with
         * redirected stdin from prev pipe. */
        if (is_last) {
            /* Redirect stdin from previous pipe */
            int saved_stdin = -1;
            if (i > 0) {
                saved_stdin = dup(STDIN_FILENO);
                /* Flush stdio stdin buffer before dup2: if the shell read the
                 * script from stdin via fgetc/fgets, the buffer may contain
                 * unread script bytes.  After dup2, fd 0 points to the pipe,
                 * but the buffered bytes would be returned first by getchar()
                 * et al., giving the builtin stale data.  fflush(stdin) on
                 * glibc discards the read buffer, preventing this stale-read
                 * bug in builtins that use stdio (e.g. tr with getchar()). */
                fflush(stdin);
                clearerr(stdin);
                dup2(pipes[i - 1][0], STDIN_FILENO);
                close(pipes[i - 1][0]);
                close(pipes[i - 1][1]);
            }

            /* Close all remaining write ends */
            for (int k = 0; k < i - 1; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }

            int rc = exec_node(sh, stages[i]);

            if (saved_stdin >= 0) {
                dup2(saved_stdin, STDIN_FILENO);
                close(saved_stdin);
            }

            /* Wait for all children */
            int last_rc = rc;
            for (int k = 0; k < nstages - 1; k++) {
                int st;
                waitpid(pids[k], &st, 0);
                int krc = WIFEXITED(st) ? WEXITSTATUS(st) :
                          (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
                if (sh->opt_pipefail && krc != 0)
                    last_rc = krc;
            }

            sh->last_exit = last_rc;
            return last_rc;
        }

        /* Fork a child for stages 0..n-2 */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            /* Child: wire up stdin from previous pipe */
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            /* Wire up stdout to next pipe */
            dup2(pipes[i][1], STDOUT_FILENO);

            /* Close all pipe fds */
            for (int k = 0; k < nstages - 1; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }

            int rc = exec_node(sh, stages[i]);
            fflush(NULL);   /* flush all stdio buffers before _exit */
            _exit(rc);
        }

        pids[i] = pid;
    }

    /* Close all pipe ends in parent */
    for (int i = 0; i < nstages - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Wait for all children (last stage handled above in-process path,
     * but in case we don't reach it — shouldn't happen) */
    int last_rc = 0;
    for (int i = 0; i < nstages - 1; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        int krc = WIFEXITED(st) ? WEXITSTATUS(st) :
                  (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
        if (sh->opt_pipefail && krc != 0)
            last_rc = krc;
        else if (i == nstages - 2)
            last_rc = krc;
    }

    sh->last_exit = last_rc;
    return last_rc;
}

/* -------------------------------------------------------------------------
 * exec_node
 * ------------------------------------------------------------------------- */

int exec_node(shell_ctx_t *sh, node_t *node)
{
    if (unlikely(!node)) return 0;

    int rc = 0;

    switch (node->type) {

    case N_CMD:
        rc = exec_simple_cmd(sh, node->u.cmd.words,
                             node->u.cmd.assigns,
                             node->u.cmd.redirs);
        break;

    case N_PIPE:
        rc = exec_pipeline(sh, node);
        break;

    case N_AND: {
        int save_cond = sh->in_cond;
        sh->in_cond = 1;
        rc = exec_node(sh, node->u.binary.left);
        sh->in_cond = save_cond;
        sh->and_or_exempt = 0;
        if (rc == 0) {
            rc = exec_node(sh, node->u.binary.right);
        } else {
            /* Left side failed (short-circuit): exempt the list result from -e */
            sh->and_or_exempt = 1;
        }
        break;
    }

    case N_OR: {
        int save_cond = sh->in_cond;
        sh->in_cond = 1;
        rc = exec_node(sh, node->u.binary.left);
        sh->in_cond = save_cond;
        sh->and_or_exempt = 0;
        if (rc != 0) {
            rc = exec_node(sh, node->u.binary.right);
        }
        /* If left succeeded: rc=0, no errexit risk anyway */
        break;
    }

    case N_NOT: {
        int save_cond = sh->in_cond;
        sh->in_cond = 1;
        rc = exec_node(sh, node->u.binary.left);
        sh->in_cond = save_cond;
        rc = (rc == 0) ? 1 : 0;
        break;
    }

    case N_SEQ: {
        rc = exec_node(sh, node->u.binary.left);
        if (rc >= FLOW_BREAK) break;   /* propagate flow control; don't touch last_exit */
        sh->last_exit = rc;
        int seq_exempt = sh->and_or_exempt;
        sh->and_or_exempt = 0;
        if (unlikely(sh->opt_e && rc != 0 && !sh->in_cond && !seq_exempt)) break;
        rc = exec_node(sh, node->u.binary.right);
        break;
    }

    case N_ASYNC: {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            rc = 1;
            break;
        }
        if (pid == 0) {
            int r = exec_node(sh, node->u.binary.left);
            fflush(NULL);
            _exit(r);
        }
        sh->last_bg_pid = pid;
        job_add(&sh->jobs, pid);
        rc = 0;
        break;
    }

    case N_SUBSHELL: {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            rc = 1;
            break;
        }
        if (pid == 0) {
            /* Apply any redirections on the subshell node */
            redirect_ctx_t rctx;
            rctx.saved = NULL;
            rctx.error = 0;
            if (node->u.redir_node.redirs)
                redirect_apply(sh, node->u.redir_node.redirs, &rctx);
            int r = exec_node(sh, node->u.redir_node.body);
            sh->last_exit = r;
            /* Fire EXIT trap if set (POSIX: subshell EXIT trap runs on exit) */
            const char *exit_act = sh->traps[0].action;
            if (exit_act != SHELL_TRAP_DEFAULT && exit_act[0] != '\0') {
                sh->traps[0].action = SHELL_TRAP_DEFAULT;
                shell_run_string(sh, exit_act);
            }
            fflush(NULL);
            _exit(r);
        }
        int status;
        waitpid(pid, &status, 0);
        rc = WIFEXITED(status)   ? WEXITSTATUS(status) :
             WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
        break;
    }

    case N_BRACE: {
        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        if (node->u.redir_node.redirs)
            redirect_apply(sh, node->u.redir_node.redirs, &rctx);
        rc = exec_node(sh, node->u.redir_node.body);
        if (node->u.redir_node.redirs)
            redirect_restore(&rctx);
        break;
    }

    case N_IF: {
        int save_cond = sh->in_cond;
        sh->in_cond = 1;
        int cond = exec_node(sh, node->u.if_node.cond);
        sh->in_cond = save_cond;
        if (cond == 0) {
            rc = exec_node(sh, node->u.if_node.then_b);
        } else if (node->u.if_node.elif_chain) {
            rc = exec_node(sh, node->u.if_node.elif_chain);
        } else if (node->u.if_node.else_b) {
            rc = exec_node(sh, node->u.if_node.else_b);
        } else {
            rc = 0;
        }
        break;
    }

    case N_WHILE: {
        rc = 0;
        for (;;) {
            int save_cond = sh->in_cond;
            sh->in_cond = 1;
            int cond = exec_node(sh, node->u.loop.cond);
            sh->in_cond = save_cond;
            if (cond != 0) break;
            rc = exec_node(sh, node->u.loop.body);
            if (rc == FLOW_BREAK) {
                if (sh->break_level > 0) { sh->break_level--; break; }
                rc = 0; break;
            }
            if (rc == FLOW_CONTINUE) {
                if (sh->break_level > 0) { sh->break_level--; break; }
                rc = 0; continue;
            }
            if (sh->opt_e && rc != 0) break;
        }
        break;
    }

    case N_UNTIL: {
        rc = 0;
        for (;;) {
            int save_cond = sh->in_cond;
            sh->in_cond = 1;
            int cond = exec_node(sh, node->u.loop.cond);
            sh->in_cond = save_cond;
            if (cond == 0) break;
            rc = exec_node(sh, node->u.loop.body);
            if (rc == FLOW_BREAK) {
                if (sh->break_level > 0) { sh->break_level--; break; }
                rc = 0; break;
            }
            if (rc == FLOW_CONTINUE) {
                if (sh->break_level > 0) { sh->break_level--; break; }
                rc = 0; continue;
            }
            if (sh->opt_e && rc != 0) break;
        }
        break;
    }

    case N_FOR: {
        char **wlist;
        if (node->u.for_node.words) {
            wlist = expand_words(sh, node->u.for_node.words);
        } else {
            /* `for x; do` with no `in` clause: iterate $@ */
            wlist = arena_alloc(&sh->scratch_arena,
                                (size_t)(sh->positional_n + 1) * sizeof(char *));
            for (int i = 0; i < sh->positional_n; i++)
                wlist[i] = sh->positional[i];
            wlist[sh->positional_n] = NULL;
        }
        rc = 0;
        for (int i = 0; wlist && wlist[i]; i++) {
            vars_set(&sh->vars, node->u.for_node.var, wlist[i]);
            rc = exec_node(sh, node->u.for_node.body);
            if (rc == FLOW_BREAK) {
                if (sh->break_level > 0) { sh->break_level--; break; }
                rc = 0; break;
            }
            if (rc == FLOW_CONTINUE) {
                if (sh->break_level > 0) { sh->break_level--; break; }
                rc = 0; continue;
            }
            if (sh->opt_e && rc != 0) break;
        }
        break;
    }

    case N_CASE: {
        char *word = expand_word(sh, node->u.case_node.word);
        rc = 0;
        for (case_item_t *item = node->u.case_node.items; item; item = item->next) {
            int matched = 0;
            for (int pi = 0; item->patterns[pi]; pi++) {
                char *pat = expand_word(sh, item->patterns[pi]);
                if (fnmatch(pat, word, 0) == 0 ||
                    strcmp(pat, "*") == 0) {
                    matched = 1;
                    break;
                }
            }
            if (matched) {
                rc = exec_node(sh, item->body);
                break;
            }
        }
        break;
    }

    case N_FUNC:
        func_register(sh, node->u.func.name, node->u.func.body);
        rc = 0;
        break;

    case N_REDIR: {
        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        redirect_apply(sh, node->u.redir_node.redirs, &rctx);
        rc = exec_node(sh, node->u.redir_node.body);
        redirect_restore(&rctx);
        break;
    }

    default:
        rc = 0;
        break;
    }

    if (rc < FLOW_BREAK)
        sh->last_exit = rc;
    return rc;
}

/* -------------------------------------------------------------------------
 * Shell built-in implementations
 * ------------------------------------------------------------------------- */

static int exec_builtin_colon(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    return 0;
}

static int exec_builtin_true(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    return 0;
}

static int exec_builtin_false(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    return 1;
}

static int exec_builtin_exit(shell_ctx_t *sh, int argc, char **argv)
{
    int code = sh->last_exit;
    if (argc >= 2)
        code = atoi(argv[1]);
    /* Fire EXIT trap (traps[0]) before exiting; clear first to prevent re-entry */
    const char *exit_action = sh->traps[0].action;
    if (exit_action != SHELL_TRAP_DEFAULT && exit_action[0] != '\0') {
        sh->traps[0].action = SHELL_TRAP_DEFAULT;
        shell_run_string(sh, exit_action);
    }
    exit(code);
    return code; /* unreachable */
}

static int exec_builtin_set(shell_ctx_t *sh, int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '-' && arg[1] != '\0') {
            if (arg[1] == '-' && arg[2] == '\0') { i++; break; } /* -- */
            for (int k = 1; arg[k]; k++) {
                switch (arg[k]) {
                case 'e': sh->opt_e = 1; break;
                case 'u': sh->opt_u = 1; break;
                case 'x': sh->opt_x = 1; break;
                case 'f': sh->opt_f = 1; break;
                case 'n': sh->opt_n = 1; break;
                case 'o':
                    if (i + 1 < argc && strcmp(argv[i+1], "pipefail") == 0) {
                        sh->opt_pipefail = 1; i++;
                    }
                    break;
                default: break;
                }
            }
        } else if (arg[0] == '+' && arg[1] != '\0') {
            for (int k = 1; arg[k]; k++) {
                switch (arg[k]) {
                case 'e': sh->opt_e = 0; break;
                case 'u': sh->opt_u = 0; break;
                case 'x': sh->opt_x = 0; break;
                case 'f': sh->opt_f = 0; break;
                case 'n': sh->opt_n = 0; break;
                case 'o':
                    if (i + 1 < argc && strcmp(argv[i+1], "pipefail") == 0) {
                        sh->opt_pipefail = 0; i++;
                    }
                    break;
                default: break;
                }
            }
        } else {
            break; /* first non-flag arg: positionals start here */
        }
    }
    /* i now points to first positional arg (or argc if none given) */
    if (i < argc) {
        int n = argc - i;
        char **pos = arena_alloc(&sh->parse_arena, (size_t)(n + 1) * sizeof(char *));
        for (int j = 0; j < n; j++)
            pos[j] = argv[i + j];
        pos[n] = NULL;
        sh->positional   = pos;
        sh->positional_n = n;
    }
    return 0;
}

static int exec_builtin_export(shell_ctx_t *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t nlen = (size_t)(eq - argv[i]);
            char *name  = strndup(argv[i], nlen);
            if (name) {
                vars_set(&sh->vars, name, eq + 1);
                vars_export(&sh->vars, name);
                free(name);
            }
        } else {
            /* export NAME — mark existing variable for export */
            if (vars_get(&sh->vars, argv[i]) == NULL)
                vars_set(&sh->vars, argv[i], "");
            vars_export(&sh->vars, argv[i]);
        }
    }
    return 0;
}

static int exec_builtin_unset(shell_ctx_t *sh, int argc, char **argv)
{
    int func_mode = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (argv[i][0] == '-') {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == 'f') func_mode = 1;
                else if (*p == 'v') func_mode = 0;
            }
        } else {
            break;
        }
    }
    int rc = 0;
    for (; i < argc; i++) {
        if (func_mode)
            func_unregister(sh, argv[i]);
        else if (vars_unset(&sh->vars, argv[i]) != 0)
            rc = 1;
    }
    return rc;
}

static int exec_builtin_readonly(shell_ctx_t *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t nlen = (size_t)(eq - argv[i]);
            char *name  = strndup(argv[i], nlen);
            if (name) {
                vars_set(&sh->vars, name, eq + 1);
                vars_readonly(&sh->vars, name);
                free(name);
            }
        } else {
            vars_readonly(&sh->vars, argv[i]);
        }
    }
    return 0;
}

static int exec_builtin_cd(shell_ctx_t *sh, int argc, char **argv)
{
    const char *dir;
    if (argc < 2) {
        dir = vars_get(&sh->vars, "HOME");
        if (!dir) {
            fprintf(stderr, "silex: cd: HOME not set\n");
            return 1;
        }
    } else {
        dir = argv[1];
    }

    if (chdir(dir) != 0) {
        perror(dir);
        return 1;
    }

    /* Update $PWD */
    char pwd[4096];
    if (getcwd(pwd, sizeof(pwd)))
        vars_set(&sh->vars, "PWD", pwd);

    return 0;
}

static int exec_builtin_shift(shell_ctx_t *sh, int argc, char **argv)
{
    int n = 1;
    if (argc >= 2) n = atoi(argv[1]);
    if (n < 0 || n > sh->positional_n) {
        fprintf(stderr, "silex: shift: %d: too many\n", n);
        return 1;
    }
    sh->positional   += n;
    sh->positional_n -= n;
    return 0;
}

static int exec_builtin_eval(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc < 2) return 0;

    strbuf_t sb;
    sb_init(&sb, 128);
    for (int i = 1; i < argc; i++) {
        if (i > 1) sb_appendc(&sb, ' ');
        sb_append(&sb, argv[i]);
    }
    int rc = shell_run_string(sh, sb_str(&sb));
    sb_free(&sb);
    return rc;
}

static int exec_builtin_exec_cmd(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc < 2) return 0;
    (void)sh;
    vars_export_env(&sh->vars);
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 127;
}

static int exec_builtin_trap(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc < 2) return 0;

    const char *action = argv[1];
    for (int i = 2; i < argc; i++) {
        int sig = atoi(argv[i]);
        if (sig == 0 && argv[i][0] != '0') {
            /* Try signal name */
            if (strcmp(argv[i], "EXIT") == 0) sig = 0;
            else if (strcmp(argv[i], "INT") == 0)  sig = SIGINT;
            else if (strcmp(argv[i], "TERM") == 0) sig = SIGTERM;
            else if (strcmp(argv[i], "HUP") == 0)  sig = SIGHUP;
            else if (strcmp(argv[i], "QUIT") == 0) sig = SIGQUIT;
            else if (strcmp(argv[i], "PIPE") == 0) sig = SIGPIPE;
            else if (strcmp(argv[i], "CHLD") == 0) sig = SIGCHLD;
            else if (strcmp(argv[i], "USR1") == 0) sig = SIGUSR1;
            else if (strcmp(argv[i], "USR2") == 0) sig = SIGUSR2;
            else continue;
        }

        if (sig < 0 || sig >= NSIG) continue;

        if (strcmp(action, "-") == 0) {
            sh->traps[sig].action = SHELL_TRAP_DEFAULT;
        } else if (strcmp(action, "") == 0) {
            sh->traps[sig].action = SHELL_TRAP_IGNORE;
        } else {
            sh->traps[sig].action = arena_strdup(&sh->parse_arena, action);
        }

        /* Install the signal handler */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        if (sh->traps[sig].action == NULL) {
            sa.sa_handler = SIG_DFL;
        } else if (sh->traps[sig].action[0] == '\0') {
            sa.sa_handler = SIG_IGN;
        } else {
            sa.sa_handler = SIG_DFL; /* handler is run in shell_signal_handler */
            extern void shell_signal_handler(int);
            sa.sa_handler = shell_signal_handler;
        }
        sigaction(sig, &sa, NULL);
    }
    return 0;
}

static int exec_builtin_read(shell_ctx_t *sh, int argc, char **argv)
{
    /* read [-r] VAR... */
    int raw = 0;
    int optind = 1;
    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-r") == 0) { raw = 1; optind++; }
        else if (strcmp(argv[optind], "--") == 0) { optind++; break; }
        else break;
    }

    strbuf_t line;
    sb_init(&line, 256);

    int c;
    int cont = 1;
    while (cont) {
        c = fgetc(stdin);
        if (c == EOF) { cont = 0; break; }
        if (c == '\n') break;
        if (!raw && c == '\\') {
            int nc = fgetc(stdin);
            if (nc == '\n') continue;  /* line continuation */
            sb_appendc(&line, (char)c);
            if (nc != EOF) sb_appendc(&line, (char)nc);
        } else {
            sb_appendc(&line, (char)c);
        }
    }

    /* Split into variables */
    const char *ifs = vars_get(&sh->vars, "IFS");
    if (!ifs) ifs = " \t\n";

    int nvars = argc - optind;
    if (nvars == 0) {
        /* No variable names: discard */
        sb_free(&line);
        return (c == EOF && sb_len(&line) == 0) ? 1 : 0;
    }

    char *linecopy = strdup(sb_str(&line));
    sb_free(&line);
    if (!linecopy) return 1;

    char *p = linecopy;
    for (int vi = optind; vi < argc; vi++) {
        /* Trim leading IFS on all but last */
        if (vi < argc - 1) {
            while (*p && strchr(ifs, (unsigned char)*p)) p++;
        }
        if (vi == argc - 1) {
            /* Last var gets rest of line (stripped of leading IFS if >1 var) */
            if (argc - optind > 1) {
                while (*p && strchr(ifs, (unsigned char)*p)) p++;
            }
            vars_set(&sh->vars, argv[vi], p);
            break;
        }
        /* Find end of field */
        char *start = p;
        while (*p && !strchr(ifs, (unsigned char)*p)) p++;
        char saved = *p;
        *p = '\0';
        vars_set(&sh->vars, argv[vi], start);
        *p = saved;
        if (*p) p++;
    }

    free(linecopy);
    return (c == EOF) ? 1 : 0;
}

static int exec_builtin_printf_sh(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;
    if (argc < 2) return 0;

    const char *fmt = argv[1];
    int argi = 2;
    const char *p = fmt;

    while (*p) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n':  putchar('\n'); break;
            case 't':  putchar('\t'); break;
            case 'r':  putchar('\r'); break;
            case '\\': putchar('\\'); break;
            case '0': {
                /* Octal escape */
                unsigned char oc = 0;
                int nd = 0;
                while (nd < 3 && p[1] >= '0' && p[1] <= '7') {
                    oc = (unsigned char)(oc * 8 + (p[1] - '0'));
                    p++; nd++;
                }
                putchar((int)oc);
                break;
            }
            default: putchar('\\'); putchar(*p); break;
            }
            p++;
        } else if (*p == '%' && p[1] != '\0') {
            p++;
            switch (*p) {
            case 's':
                if (argi < argc) fputs(argv[argi++], stdout);
                break;
            case 'd': {
                long v = argi < argc ? atol(argv[argi++]) : 0;
                printf("%ld", v);
                break;
            }
            case 'f': {
                double v = argi < argc ? atof(argv[argi++]) : 0.0;
                printf("%f", v);
                break;
            }
            case '%':
                putchar('%');
                break;
            default:
                putchar('%');
                putchar(*p);
                break;
            }
            p++;
        } else {
            putchar(*p++);
        }
    }

    return 0;
}

/* Minimal test / [ implementation */
static int exec_builtin_test(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;
    int bracket = (strcmp(argv[0], "[") == 0);

    /* Strip trailing ] for [ invocation */
    if (bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "silex: test: missing ]\n");
            return 2;
        }
        argc--;
    }

    int testargc = argc - 1;
    char **testargs = argv + 1;

    if (testargc == 0) return 1;

    if (testargc == 1) {
        return testargs[0][0] ? 0 : 1;
    }

    if (testargc == 2) {
        const char *op = testargs[0];
        const char *a  = testargs[1];
        if (strcmp(op, "-z") == 0) return (a[0] == '\0') ? 0 : 1;
        if (strcmp(op, "-n") == 0) return (a[0] != '\0') ? 0 : 1;
        if (strcmp(op, "-e") == 0) { struct stat st; return (stat(a, &st) == 0) ? 0 : 1; }
        if (strcmp(op, "-f") == 0) { struct stat st; return (stat(a, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1; }
        if (strcmp(op, "-d") == 0) { struct stat st; return (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1; }
        if (strcmp(op, "-r") == 0) return (access(a, R_OK) == 0) ? 0 : 1;
        if (strcmp(op, "-w") == 0) return (access(a, W_OK) == 0) ? 0 : 1;
        if (strcmp(op, "-x") == 0) return (access(a, X_OK) == 0) ? 0 : 1;
        if (strcmp(op, "-s") == 0) { struct stat st; return (stat(a, &st) == 0 && st.st_size > 0) ? 0 : 1; }
        if (strcmp(op, "!") == 0)  return (a[0] == '\0') ? 0 : 1; /* ! "" */
        return 1;
    }

    if (testargc == 3) {
        const char *a  = testargs[0];
        const char *op = testargs[1];
        const char *b  = testargs[2];

        if (strcmp(op, "=")  == 0) return (strcmp(a, b) == 0) ? 0 : 1;
        if (strcmp(op, "!=") == 0) return (strcmp(a, b) != 0) ? 0 : 1;
        if (strcmp(op, "-eq") == 0) return (atol(a) == atol(b)) ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return (atol(a) != atol(b)) ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return (atol(a) <  atol(b)) ? 0 : 1;
        if (strcmp(op, "-le") == 0) return (atol(a) <= atol(b)) ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return (atol(a) >  atol(b)) ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return (atol(a) >= atol(b)) ? 0 : 1;
        if (strcmp(op, "-a") == 0)  return (a[0] && b[0]) ? 0 : 1;
        if (strcmp(op, "-o") == 0)  return (a[0] || b[0]) ? 0 : 1;
        if (strcmp(op, "-nt") == 0) {
            struct stat sa, sb2;
            if (stat(a, &sa) != 0 || stat(b, &sb2) != 0) return 1;
            return (sa.st_mtime > sb2.st_mtime) ? 0 : 1;
        }
        if (strcmp(op, "-ot") == 0) {
            struct stat sa, sb2;
            if (stat(a, &sa) != 0 || stat(b, &sb2) != 0) return 1;
            return (sa.st_mtime < sb2.st_mtime) ? 0 : 1;
        }
        if (strcmp(op, "-ef") == 0) {
            struct stat sa, sb2;
            if (stat(a, &sa) != 0 || stat(b, &sb2) != 0) return 1;
            return (sa.st_dev == sb2.st_dev && sa.st_ino == sb2.st_ino) ? 0 : 1;
        }
    }

    /* Unary with 3 args: ! -z str */
    if (testargc == 3 && strcmp(testargs[0], "!") == 0) {
        /* Create sub-invocation */
        char *subargs[3] = { argv[0], testargs[1], testargs[2] };
        int sub = exec_builtin_test(sh, 3, subargs);
        return sub ? 0 : 1;
    }

    return 1;
}

static int exec_builtin_local(shell_ctx_t *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t nlen = (size_t)(eq - argv[i]);
            char *name  = strndup(argv[i], nlen);
            if (name) {
                vars_set_local(&sh->vars, name, eq + 1);
                free(name);
            }
        } else {
            vars_set_local(&sh->vars, argv[i], "");
        }
    }
    return 0;
}

static int exec_builtin_return(shell_ctx_t *sh, int argc, char **argv)
{
    int code = sh->last_exit;
    if (argc >= 2) code = atoi(argv[1]);
    sh->last_exit = code;
    return FLOW_RETURN;
}

static int exec_builtin_wait(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc >= 2) {
        pid_t pid = (pid_t)atol(argv[1]);
        return job_wait(&sh->jobs, pid);
    }
    /* Wait for all background jobs */
    int rc = 0;
    for (job_t *j = sh->jobs.head; j; j = j->next) {
        if (!j->done) {
            int r = job_wait(&sh->jobs, j->pid);
            rc = r;
        }
    }
    return rc;
}

static int exec_builtin_source(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "silex: .: filename argument required\n");
        return 1;
    }
    return shell_run_file(sh, argv[1]);
}

static int exec_builtin_break(shell_ctx_t *sh, int argc, char **argv)
{
    int n = (argc >= 2) ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    sh->break_level = n - 1;  /* loops decrement; propagate if > 0 */
    return FLOW_BREAK;
}

static int exec_builtin_continue(shell_ctx_t *sh, int argc, char **argv)
{
    int n = (argc >= 2) ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    sh->break_level = n - 1;
    return FLOW_CONTINUE;
}

static int exec_builtin_umask(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;
    int symbolic = 0;
    int i = 1;
    if (argc >= 2 && strcmp(argv[1], "-S") == 0) {
        symbolic = 1; i = 2;
    }
    if (i >= argc) {
        /* Print current umask */
        mode_t m = umask(0); umask(m);
        if (symbolic) {
            printf("u=%s%s%s,g=%s%s%s,o=%s%s%s\n",
                (m & 0400) ? "" : "r", (m & 0200) ? "" : "w", (m & 0100) ? "" : "x",
                (m & 0040) ? "" : "r", (m & 0020) ? "" : "w", (m & 0010) ? "" : "x",
                (m & 0004) ? "" : "r", (m & 0002) ? "" : "w", (m & 0001) ? "" : "x");
        } else {
            printf("%04o\n", (unsigned)m);
        }
        return 0;
    }
    /* Set umask */
    char *end;
    unsigned long val = strtoul(argv[i], &end, 8);
    if (*end != '\0' || val > 0777) {
        fprintf(stderr, "silex: umask: invalid mode: %s\n", argv[i]);
        return 1;
    }
    umask((mode_t)val);
    return 0;
}

static int exec_builtin_command(shell_ctx_t *sh, int argc, char **argv)
{
    /* command [-v|-V|-p] [--] name [arg...] */
    int describe = 0, verbose = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-v") == 0)      { describe = 1; i++; }
        else if (strcmp(argv[i], "-V") == 0) { verbose  = 1; i++; }
        else if (strcmp(argv[i], "-p") == 0) { i++; } /* ignore -p */
        else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else break;
    }
    if (i >= argc) return 0;
    const char *name = argv[i];

    if (describe || verbose) {
        /* Check shell builtin */
        if (find_shell_builtin(name)) {
            if (verbose) printf("%s is a shell builtin\n", name);
            else         printf("%s\n", name);
            return 0;
        }
        /* Check applet table */
        if (find_applet(name)) {
            if (verbose) printf("%s is a silex builtin\n", name);
            else         printf("%s\n", name);
            return 0;
        }
        /* Search PATH */
        const char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = strdup(path_env);
            if (path_copy) {
                char *dir = strtok(path_copy, ":");
                while (dir) {
                    char full[PATH_MAX];
                    snprintf(full, sizeof(full), "%s/%s", dir, name);
                    struct stat st;
                    if (stat(full, &st) == 0 && (st.st_mode & S_IXUSR)) {
                        if (verbose) printf("%s is %s\n", name, full);
                        else         printf("%s\n", full);
                        free(path_copy);
                        return 0;
                    }
                    dir = strtok(NULL, ":");
                }
                free(path_copy);
            }
        }
        if (verbose) fprintf(stderr, "silex: command: %s: not found\n", name);
        return 1;
    }

    /* Execute name bypassing shell functions (builtins still apply) */
    return exec_simple_cmd(sh, argv + i, NULL, NULL);
}

static int exec_builtin_type(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        if (find_shell_builtin(name)) {
            printf("%s is a shell builtin\n", name);
            continue;
        }
        if (find_applet(name)) {
            printf("%s is a silex builtin\n", name);
            continue;
        }
        /* Check PATH */
        const char *path_env = getenv("PATH");
        int found = 0;
        if (path_env) {
            char *path_copy = strdup(path_env);
            if (path_copy) {
                char *dir = strtok(path_copy, ":");
                while (dir) {
                    char full[PATH_MAX];
                    snprintf(full, sizeof(full), "%s/%s", dir, name);
                    struct stat st;
                    if (stat(full, &st) == 0 && (st.st_mode & S_IXUSR)) {
                        printf("%s is %s\n", name, full);
                        found = 1;
                        break;
                    }
                    dir = strtok(NULL, ":");
                }
                free(path_copy);
            }
        }
        if (!found) {
            fprintf(stderr, "silex: type: %s: not found\n", name);
            ret = 1;
        }
    }
    return ret;
}

static int exec_builtin_getopts(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "silex: getopts: usage: getopts optstring name [arg...]\n");
        return 1;
    }
    const char *optstring = argv[1];
    const char *varname   = argv[2];
    int silent = (optstring[0] == ':');
    if (silent) optstring++;

    /* Get OPTIND (1-based; default 1) */
    const char *optind_str = vars_get(&sh->vars, "OPTIND");
    int optind = optind_str ? atoi(optind_str) : 1;
    if (optind < 1) optind = 1;

    /* Build args array: argv[3..] if given, else sh->positional */
    const char **args;
    int nargs;
    if (argc > 3) {
        args  = (const char **)(argv + 3);
        nargs = argc - 3;
    } else {
        args  = (const char **)sh->positional;
        nargs = sh->positional_n;
    }

    /* OPTPOS: sub-index within current arg (stored as "__OPTPOS" internal var) */
    const char *pos_str = vars_get(&sh->vars, "__OPTPOS");
    int optpos = pos_str ? atoi(pos_str) : 1;
    if (optpos < 1) optpos = 1;

    /* Check bounds */
    if (optind > nargs) {
        vars_set(&sh->vars, varname, "?");
        return 1;
    }
    const char *arg = args[optind - 1];

    /* Check if arg is an option */
    if (arg[0] != '-' || arg[1] == '\0') {
        vars_set(&sh->vars, varname, "?");
        return 1;
    }
    if (strcmp(arg, "--") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", optind + 1);
        vars_set(&sh->vars, "OPTIND", buf);
        vars_set(&sh->vars, "__OPTPOS", "1");
        vars_set(&sh->vars, varname, "?");
        return 1;
    }

    /* Get option char at optpos within arg */
    if ((size_t)optpos >= strlen(arg)) {
        /* Move to next arg */
        optind++;
        optpos = 1;
        if (optind > nargs) {
            vars_set(&sh->vars, varname, "?");
            return 1;
        }
        arg = args[optind - 1];
        if (arg[0] != '-' || arg[1] == '\0' || strcmp(arg, "--") == 0) {
            vars_set(&sh->vars, varname, "?");
            return 1;
        }
    }

    char opt = arg[optpos];
    char opt_str[2] = { opt, '\0' };

    /* Find opt in optstring */
    const char *p = strchr(optstring, opt);
    if (!p) {
        /* Unknown option */
        if (!silent) fprintf(stderr, "silex: getopts: illegal option -- %c\n", opt);
        vars_set(&sh->vars, varname, "?");
        vars_set(&sh->vars, "OPTARG", opt_str);
    } else {
        vars_set(&sh->vars, varname, opt_str);
        if (p[1] == ':') {
            /* Requires argument */
            if (arg[optpos + 1] != '\0') {
                vars_set(&sh->vars, "OPTARG", arg + optpos + 1);
                optind++;
                optpos = 1;
            } else {
                optind++;
                optpos = 1;
                if (optind > nargs) {
                    if (!silent) fprintf(stderr, "silex: getopts: option requires an argument -- %c\n", opt);
                    vars_set(&sh->vars, varname, silent ? ":" : "?");
                    vars_set(&sh->vars, "OPTARG", opt_str);
                } else {
                    vars_set(&sh->vars, "OPTARG", args[optind - 1]);
                    optind++;
                }
            }
        } else {
            vars_set(&sh->vars, "OPTARG", "");
            optpos++;
            if ((size_t)optpos >= strlen(arg)) {
                optind++;
                optpos = 1;
            }
        }
    }

    /* Update OPTIND and __OPTPOS */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", optind);
    vars_set(&sh->vars, "OPTIND", buf);
    snprintf(buf, sizeof(buf), "%d", optpos);
    vars_set(&sh->vars, "__OPTPOS", buf);

    return 0;
}

