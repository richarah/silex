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
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

/* Not static: shell_free() calls this too (see shell.c). */
void positional_free(shell_ctx_t *sh);

static int exec_builtin_kill(shell_ctx_t *sh, int argc, char **argv);
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
static int exec_builtin_times(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_source(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_break(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_continue(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_umask(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_command(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_type(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_getopts(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_alias(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_unalias(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_hash(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_jobs(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_fg(shell_ctx_t *sh, int argc, char **argv);
static int exec_builtin_bg(shell_ctx_t *sh, int argc, char **argv);
static job_t *parse_jobspec(shell_ctx_t *sh, const char *spec);

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
    { "times",    exec_builtin_times     },
    { ".",        exec_builtin_source    },
    { "source",   exec_builtin_source    },
    { "break",    exec_builtin_break     },
    { "continue", exec_builtin_continue  },
    { "umask",    exec_builtin_umask     },
    { "command",  exec_builtin_command   },
    { "type",     exec_builtin_type      },
    { "getopts",  exec_builtin_getopts   },
    { "alias",    exec_builtin_alias     },
    { "unalias",  exec_builtin_unalias   },
    { "hash",     exec_builtin_hash      },
    { "kill",     exec_builtin_kill      },
    { "jobs",     exec_builtin_jobs      },
    { "fg",       exec_builtin_fg        },
    { "bg",       exec_builtin_bg        },
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

/* Map a signal name (with or without the SIG prefix, any case) to its number,
 * or -1 if unknown. Shared by `trap` and `kill`. "EXIT"/"0" is signal 0. */
static int signal_from_name(const char *name)
{
    if (name[0] == 'S' && name[1] == 'I' && name[2] == 'G')
        name += 3;  /* accept SIGINT as well as INT */
    static const struct { const char *n; int s; } tbl[] = {
        { "EXIT", 0 }, { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT },
        { "ILL", SIGILL }, { "ABRT", SIGABRT }, { "FPE", SIGFPE },
        { "KILL", SIGKILL }, { "SEGV", SIGSEGV }, { "PIPE", SIGPIPE },
        { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "USR1", SIGUSR1 },
        { "USR2", SIGUSR2 }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT },
        { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "TTIN", SIGTTIN },
        { "TTOU", SIGTTOU }, { "BUS", SIGBUS }, { "TRAP", SIGTRAP },
        { "URG", SIGURG }, { "WINCH", SIGWINCH },
        { NULL, 0 }
    };
    for (int i = 0; tbl[i].n; i++)
        if (strcasecmp(name, tbl[i].n) == 0)
            return tbl[i].s;
    return -1;
}

/* Reverse of signal_from_name: signal number -> bare name, or NULL if unknown. */
static const char *signal_number_to_name(int sig)
{
    switch (sig) {
    case SIGHUP:  return "HUP";
    case SIGINT:  return "INT";
    case SIGQUIT: return "QUIT";
    case SIGILL:  return "ILL";
    case SIGTRAP: return "TRAP";
    case SIGABRT: return "ABRT";
    case SIGBUS:  return "BUS";
    case SIGFPE:  return "FPE";
    case SIGKILL: return "KILL";
    case SIGUSR1: return "USR1";
    case SIGSEGV: return "SEGV";
    case SIGUSR2: return "USR2";
    case SIGPIPE: return "PIPE";
    case SIGALRM: return "ALRM";
    case SIGTERM: return "TERM";
    case SIGCHLD: return "CHLD";
    case SIGCONT: return "CONT";
    case SIGSTOP: return "STOP";
    case SIGTSTP: return "TSTP";
    case SIGTTIN: return "TTIN";
    case SIGTTOU: return "TTOU";
    default:      return NULL;
    }
}

/* Check if a command name is a POSIX special builtin.
 * Special builtins: break, :, continue, ., eval, exec, exit, export,
 * readonly, return, set, shift, times, trap, unset.
 * Special builtins must cause the shell to exit on certain errors
 * (e.g., redirect failures, readonly violations) in non-interactive mode. */
static int is_special_builtin(const char *name)
{
    return (strcmp(name, ":") == 0 ||
            strcmp(name, ".") == 0 ||
            strcmp(name, "break") == 0 ||
            strcmp(name, "continue") == 0 ||
            strcmp(name, "eval") == 0 ||
            strcmp(name, "exec") == 0 ||
            strcmp(name, "exit") == 0 ||
            strcmp(name, "export") == 0 ||
            strcmp(name, "readonly") == 0 ||
            strcmp(name, "return") == 0 ||
            strcmp(name, "set") == 0 ||
            strcmp(name, "shift") == 0 ||
            strcmp(name, "times") == 0 ||
            strcmp(name, "trap") == 0 ||
            strcmp(name, "unset") == 0);
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
 * Alias registry helpers
 * Store alias definitions as (name, value) pairs.
 * sh->aliases[256] is used as a simple open-addressing hash.
 * ------------------------------------------------------------------------- */

typedef struct alias_entry {
    char        *name;
    char        *value;
    struct alias_entry *next;
} alias_entry_t;

static void alias_register(shell_ctx_t *sh, const char *name, const char *value)
{
    unsigned int idx = func_hash(name);  /* Reuse func_hash for aliases */
    alias_entry_t *e = sh->aliases[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) {
            /* Update existing alias */
            e->value = arena_strdup(&sh->parse_arena, value);
            return;
        }
        e = e->next;
    }
    /* Allocate new entry in arena */
    alias_entry_t *ne = arena_alloc(&sh->parse_arena, sizeof(alias_entry_t));
    ne->name  = arena_strdup(&sh->parse_arena, name);
    ne->value = arena_strdup(&sh->parse_arena, value);
    ne->next  = sh->aliases[idx];
    sh->aliases[idx] = ne;
}

static const char *alias_lookup(shell_ctx_t *sh, const char *name)
{
    unsigned int idx = func_hash(name);
    alias_entry_t *e = sh->aliases[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) return e->value;
        e = e->next;
    }
    return NULL;
}

static void alias_unregister(shell_ctx_t *sh, const char *name)
{
    unsigned int idx = func_hash(name);
    alias_entry_t **pp = (alias_entry_t **)&sh->aliases[idx];
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            *pp = (*pp)->next;   /* unlink (arena-allocated; not freed) */
            return;
        }
        pp = &(*pp)->next;
    }
}

static void alias_clear_all(shell_ctx_t *sh)
{
    for (int i = 0; i < 256; i++) {
        sh->aliases[i] = NULL;
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
    /* Not a leak: every failure path below frees e (see the strdup check). cppcheck
     * cannot follow the unlikely() macro into the error branch. */
    path_cache_entry_t *e = malloc(sizeof(*e));
    /* cppcheck-suppress memleak */
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

static int exec_simple_cmd_inner(shell_ctx_t *sh, char **words, char **assigns,
                                 redir_t *redirs);

/*
 * Run a simple command, restoring any redirect targets the pre-expansion below
 * overwrote.
 *
 * exec_simple_cmd_inner() pre-expands each redirect target and writes the result
 * back into r->target. But `r` is part of the PARSED AST, which is reused on
 * every pass, so that write is destructive: the second time a command runs, the
 * "target" it expands is the first run's expansion. Inside a loop that silently
 * collapses every iteration onto the first iteration's filename --
 *
 *     for f in a b c; do echo hi > /tmp/t_$f.txt; done
 *
 * created only /tmp/t_a.txt, where dash creates t_a, t_b and t_c. A loop meant to
 * write N files wrote one, with no error.
 *
 * The write-back cannot simply be dropped: redirect.c expands r->target again
 * itself, so without it a `> $(cmd)` target would run cmd twice. Instead the AST
 * pointers are saved here and restored once the command is done, whichever of
 * the inner function's many exits it takes.
 */
int exec_simple_cmd(shell_ctx_t *sh, char **words, char **assigns, redir_t *redirs)
{
    enum { SAVED_INLINE = 16 };
    char   *inline_slots[SAVED_INLINE];
    redir_t *inline_who[SAVED_INLINE];
    char   **saved = inline_slots;
    redir_t **who  = inline_who;
    int n = 0, cap = SAVED_INLINE, heap = 0;

    for (redir_t *r = redirs; r != NULL; r = r->next) {
        if (n == cap) {                     /* rare: more than 16 redirects */
            int ncap = cap * 2;
            char **s2 = malloc((size_t)ncap * sizeof *s2);
            redir_t **w2 = malloc((size_t)ncap * sizeof *w2);
            if (!s2 || !w2) { free(s2); free(w2); break; }  /* restore what we have */
            memcpy(s2, saved, (size_t)n * sizeof *s2);
            memcpy(w2, who, (size_t)n * sizeof *w2);
            if (heap) { free(saved); free(who); }
            saved = s2; who = w2; cap = ncap; heap = 1;
        }
        /* r cannot be NULL: it is the loop variable, guarded by `r != NULL`
         * above. cppcheck 2.13 (what CI's apt ships) reports a possible null
         * dereference here anyway, once `r` has been stored into who[n]; 2.21
         * does not, which is why this only fails in CI. Suppressed rather than
         * reordered, because working around a specific version's analysis by
         * shuffling statements is guesswork that the next release would undo. */
        /* cppcheck-suppress nullPointer */
        who[n] = r; saved[n] = r->target; n++;
    }

    int rc = exec_simple_cmd_inner(sh, words, assigns, redirs);

    for (int i = 0; i < n; i++)
        who[i]->target = saved[i];
    /* Not freeing the inline arrays: `heap` is set only where saved/who were
     * reassigned to malloc'd buffers above, so this is unreachable while they
     * still point at inline_slots/inline_who. cppcheck reports the auto-variable
     * deallocation because it does not correlate the flag with the pointers. */
    /* cppcheck-suppress autovarInvalidDeallocation */
    if (heap) { free(saved); free(who); }
    return rc;
}

static int exec_simple_cmd_inner(shell_ctx_t *sh, char **words, char **assigns,
                                 redir_t *redirs)
{
    /* POSIX: expand redirections BEFORE command arguments
     * This ensures that variable assignments in redirect targets
     * (like 2>${x=redir}) happen before argument expansions (like ${x=assign}) */
    for (redir_t *r = redirs; r != NULL; r = r->next) {
        if (r->op != TOK_DLESS && r->op != TOK_DLESSDASH) {
            /* Pre-expand redirect target to trigger any variable assignments */
            char *target = expand_word(sh, r->target);
            /* Store expanded target back (will be used by redirect_apply later).
             * exec_simple_cmd() restores the AST pointer afterwards. */
            if (target && target != r->target) {
                r->target = arena_strdup(sh->scratch, target);
            }
        }
    }

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
        if (!anames || !avals) {
            free(anames); free(avals);
            fprintf(stderr, "silex: out of memory\n");
            return 1;
        }
        /* POSIX 2.9.1: with no command name, the command completes with the
         * status of the LAST command substitution performed. expand.c records
         * each one in last_cmdsub_exit; zero it first so "no substitution at
         * all" yields 0.
         *
         * This used to read sh->last_exit, which expand.c never set for a
         * command substitution -- so cmdsub_exit was always 0 and
         * `v=$(cmd); ret=$?` always saw success. */
        sh->last_cmdsub_exit = 0;
        for (int i = 0; i < nassigns; i++) {
            const char *eq = strchr(assigns[i], '=');
            if (!eq) { anames[i] = NULL; avals[i] = NULL; continue; }
            size_t nlen = (size_t)(eq - assigns[i]);
            anames[i] = strndup(assigns[i], nlen);
            /* Use expand_word_assign for assignment values - enables ~: expansion */
            avals[i]  = expand_word_assign(sh, eq + 1);
        }
        cmdsub_exit = sh->last_cmdsub_exit;
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

    /* With command: env-prefix assignments will be applied in child process
     * or function scope, not in parent shell */

    /* 2. Expand all words */
    char **expanded = expand_words(sh, words);
    int cmd_rc = 0;
    if (unlikely(!expanded || !expanded[0]))
        goto cmd_done;

    {
    const char *cmd = expanded[0];

    /* Alias expansion: check if first word is an alias */
    const char *alias_value = alias_lookup(sh, cmd);
    if (alias_value) {
        /* If alias expands to empty string, treat as no-op */
        if (alias_value[0] == '\0') {
            cmd_rc = 0;
            goto cmd_done;
        }
        /* Simple word splitting on spaces (simplified - doesn't handle quotes properly)
         * Full POSIX alias expansion would require re-lexing/parsing */
        char *alias_copy = arena_strdup(sh->scratch, alias_value);
        int alias_argc = 0;
        char *alias_words[256];  /* Max 256 words in alias expansion */

        /* Split on whitespace */
        char *p = alias_copy;
        while (*p) {
            /* Skip whitespace */
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (!*p) break;
            /* Found start of word */
            alias_words[alias_argc++] = p;
            /* Find end of word */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }

        if (alias_argc > 0) {
            /* Count original args (excluding first word which is being replaced) */
            int orig_argc = 0;
            while (expanded[orig_argc]) orig_argc++;

            /* Build new expanded array: alias_words + original args[1..] */
            int new_argc = alias_argc + (orig_argc - 1);
            char **new_expanded = arena_alloc(sh->scratch, (size_t)(new_argc + 1) * sizeof(char *));
            for (int i = 0; i < alias_argc; i++)
                new_expanded[i] = alias_words[i];
            for (int i = 1; i < orig_argc; i++)
                new_expanded[alias_argc + i - 1] = expanded[i];
            new_expanded[new_argc] = NULL;

            expanded = new_expanded;
            cmd = expanded[0];
        }
    }

    /* Count argc */
    int argc = 0;
    while (expanded[argc]) argc++;

    /* xtrace (set -x) and the SILEX_TRACE debug env both print the command,
     * post-expansion, to stderr before it runs -- this is POSIX `set -x`.
     * opt_x was previously set but never consulted, so `set -x` traced nothing.
     * PS4 supplies the prefix (POSIX; default "+ "). */
    if (sh->opt_x || sh->trace_level >= 1) {
        const char *ps4 = vars_get(&sh->vars, "PS4");
        fputs(ps4 ? ps4 : "+ ", stderr);
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
        /* If redirect failed, don't execute the command */
        if (rctx.error) {
            cmd_rc = 1;
            /* POSIX: Special builtins must cause non-interactive shell to exit on redirect error
             * EXCEPT when invoked via 'command' prefix */
            if (!sh->interactive && !sh->in_command_builtin && is_special_builtin(cmd)) {
                /* Exit immediately with error status */
                exit(1);
            }
        } else {
            cmd_rc = sfn(sh, argc, expanded);

            /* A builtin whose output could not be written has not succeeded.
             *
             * stdio buffers, so a failing write() is not seen by the builtin's
             * printf -- it surfaces at the flush, which for a builtin never
             * happens before the redirection is torn down. So `times >/dev/full`,
             * `export -p >/dev/full`, `type echo >/dev/full` and friends all
             * reported success while writing nothing. smoosh calls this class
             * "silently failing commands" and tests for it directly.
             *
             * Flush while the redirection is still applied -- after
             * redirect_restore() below, stdout points somewhere else and the
             * error is lost. EPIPE stays silent (`yes | head` is not an error).
             */
            if (fflush(stdout) != 0 || ferror(stdout)) {
                if (errno != EPIPE) {
                    fprintf(stderr, "silex: %s: write error: %s\n",
                            cmd, strerror(errno));
                    if (cmd_rc == 0)
                        cmd_rc = 1;
                }
                clearerr(stdout);
            }
        }
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
        char **old_pos   = sh->positional;
        int old_n        = sh->positional_n;
        char **old_base  = sh->positional_base;
        int old_base_n   = sh->positional_base_n;
        int old_break    = sh->break_level;
        int old_loop     = sh->loop_depth;

        sh->positional_n = argc - 1;
        sh->positional   = expanded + 1;
        /* The arguments live in the caller's expansion, so this frame owns
         * nothing yet. Clearing the base is what stops a `set --` inside the
         * function from freeing the CALLER's list, which old_pos still needs. */
        sh->positional_base   = NULL;
        sh->positional_base_n = 0;
        sh->break_level  = 0;  /* Isolate break/continue from caller */
        sh->loop_depth   = 0;  /* Functions don't inherit loop context */

        vars_push_scope(&sh->vars);
        /* Import environment-prefix variables into function scope */
        for (int i = 0; i < nassigns; i++) {
            if (anames[i])
                vars_set_local(&sh->vars, anames[i], avals[i] ? avals[i] : "");
        }
        sh->call_depth++;
        cmd_rc = exec_node(sh, fnbody);
        sh->call_depth--;
        vars_pop_scope(&sh->vars);

        /* Release anything a `set --` inside the function allocated; it dies
         * with the frame. No-op if the function never ran one. */
        positional_free(sh);
        sh->positional        = old_pos;
        sh->positional_n      = old_n;
        sh->positional_base   = old_base;
        sh->positional_base_n = old_base_n;
        sh->break_level  = old_break;
        sh->loop_depth   = old_loop;

        if (redirs) redirect_restore(&rctx);

        /* Absorb flow control: functions isolate break/continue/return from caller */
        if (cmd_rc == FLOW_RETURN) cmd_rc = sh->last_exit;
        if (cmd_rc == FLOW_BREAK || cmd_rc == FLOW_CONTINUE) cmd_rc = 0;
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
                /* Apply env-prefix assignments AFTER exporting shell vars */
                for (int i = 0; i < nassigns; i++) {
                    if (anames[i])
                        setenv(anames[i], avals[i] ? avals[i] : "", 1);
                }
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

        /* Under job control, become a new foreground process group with the
         * job-control signals back at their default, so ^Z/^C reach the command
         * rather than the shell (which ignores them). */
        if (sh->job_control) {
            setpgid(0, 0);
            if (sh->tty_fd >= 0)
                tcsetpgrp(sh->tty_fd, getpid());
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
        }

        redirect_ctx_t rctx;
        rctx.saved = NULL;
        rctx.error = 0;
        if (redirs) redirect_apply(sh, redirs, &rctx);

        /* Export all exported vars */
        vars_export_env(&sh->vars);

        /* Apply env-prefix assignments AFTER exporting shell vars */
        for (int i = 0; i < nassigns; i++) {
            if (anames[i])
                setenv(anames[i], avals[i] ? avals[i] : "", 1);
        }

        execv(exec_path, expanded);
        perror(exec_path);
        _exit(127);
    }

    /* Parent */
    {
    int status;
    int jc = sh->job_control;
    if (jc) {
        setpgid(pid, pid);                       /* mirror the child's setpgid */
        if (sh->tty_fd >= 0)
            tcsetpgrp(sh->tty_fd, pid);          /* hand the terminal to the job */
    }
    while (waitpid(pid, &status, jc ? WUNTRACED : 0) < 0 && errno == EINTR) {}
    if (jc && sh->tty_fd >= 0)
        tcsetpgrp(sh->tty_fd, sh->shell_pgid);   /* take the terminal back */
    if (WIFEXITED(status))
        cmd_rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        cmd_rc = 128 + WTERMSIG(status);
    else if (WIFSTOPPED(status)) {
        /* ^Z: the foreground command stopped. Record it as a stopped job so it
         * can be resumed with fg/bg, and report it like `[1]+ Stopped cmd`. */
        char *lbl = NULL;
        size_t len = 0;
        for (int a = 0; a < argc && expanded[a]; a++) len += strlen(expanded[a]) + 1;
        if (len && (lbl = malloc(len + 1))) {
            lbl[0] = '\0';
            for (int a = 0; a < argc && expanded[a]; a++) {
                if (a) strcat(lbl, " ");
                strcat(lbl, expanded[a]);
            }
        }
        job_t *j = job_register(&sh->jobs, pid, pid, lbl);
        free(lbl);
        if (j) {
            j->state  = JOB_STOPPED;
            j->status = status;
            fprintf(stderr, "\n[%d]+ Stopped  %s\n", j->id, j->command ? j->command : "");
        }
        cmd_rc = 128 + WSTOPSIG(status);
    }
    else
        cmd_rc = 1;
    sh->last_exit = cmd_rc;
    /* B-1: external command may have changed filesystem state — invalidate all */
    fscache_invalidate_all();
    }
    } /* end command dispatch block */

cmd_done:
    /* Free env-prefix assign storage (env was only modified in child processes) */
    for (int i = 0; i < nassigns; i++) {
        if (anames[i])
            free(anames[i]);
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
                /* fflush() on an INPUT stream is undefined behaviour in ISO C.
                 * glibc defines it as discarding the read buffer, which is
                 * exactly what we want here -- but musl and the BSDs do not, so
                 * only do it where it is defined. Everywhere else, clearerr()
                 * alone is the portable part. */
#ifdef __GLIBC__
                /* cppcheck-suppress fflushOnInputStream */
                fflush(stdin);
#endif
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

/* Release the positional list, if this frame owns it.
 *
 * Frees from positional_base, NEVER from sh->positional: `shift` advances
 * sh->positional into the middle of the array, and free() of an interior
 * pointer is undefined. For the same reason the count comes from
 * positional_base_n -- shift decrements positional_n, which would leak the
 * shifted-off strings.
 *
 * A NULL base means the list belongs to someone else (the initial list lives in
 * parse_arena; a function's arguments live in the caller's expansion), so this
 * is a no-op and must stay one.
 */
void positional_free(shell_ctx_t *sh)
{
    if (!sh->positional_base)
        return;
    for (int i = 0; i < sh->positional_base_n; i++)
        free(sh->positional_base[i]);
    free(sh->positional_base);
    sh->positional_base   = NULL;
    sh->positional_base_n = 0;
}

/* -------------------------------------------------------------------------
 * Per-loop scratch reclamation
 *
 * A loop is a single top-level command, so without this every iteration's
 * expansions pile up in the scratch arena until the loop finishes: memory grew
 * ~220 bytes per iteration and `while [ $i -lt 300000 ]` aborted at the 64 MB
 * arena cap.
 *
 * Two simpler fixes do not work:
 *   - Resetting the shared scratch arena per iteration frees memory the loop
 *     itself owns -- a `for` word list, and the positionals of an enclosing
 *     function call -- both allocated from scratch before the loop starts.
 *   - Mark/release cannot work at all: arena_alloc() scans blocks below head
 *     before growing, so allocation is not LIFO and there is no offset to
 *     rewind to.
 *
 * Instead the loop gets its own arena and points sh->scratch at it. Anything
 * allocated before the swap stays in the parent arena and is untouched; only
 * what the condition and body allocate is reclaimed each iteration. Nested
 * loops compose: each swaps to its own arena and restores the parent's on exit.
 *
 * arena_init() does not allocate (the first arena_alloc does), so a loop whose
 * body never expands anything costs nothing.
 * ------------------------------------------------------------------------- */

typedef struct {
    arena_t  arena;
    arena_t *saved;
} loop_scratch_t;

static void loop_scratch_begin(shell_ctx_t *sh, loop_scratch_t *ls)
{
    arena_init(&ls->arena, "loop");
    ls->saved = sh->scratch;
}

/* Call at the top of every iteration, before the condition is evaluated. */
static void loop_scratch_iter(shell_ctx_t *sh, loop_scratch_t *ls)
{
    sh->scratch = &ls->arena;
    arena_reset(&ls->arena);
}

/* Must run on every exit path, including break/continue and flow control. */
static void loop_scratch_end(shell_ctx_t *sh, loop_scratch_t *ls)
{
    sh->scratch = ls->saved;
    arena_free(&ls->arena);
}

/* Reconstruct a short command label for the jobs table. The AST keeps no source
 * text, so this is best-effort: simple commands and pipelines are rebuilt from
 * their words; a compound command gets a generic label. Returns a malloc'd
 * string (owned by the caller / the job entry) or NULL. */
static char *describe_node(node_t *node)
{
    if (!node) return NULL;
    switch (node->type) {
    case N_CMD: {
        size_t len = 0;
        char **w = node->u.cmd.words;
        for (int i = 0; w && w[i]; i++)
            len += strlen(w[i]) + 1;
        if (len == 0) return NULL;
        char *s = malloc(len + 1);
        if (!s) return NULL;
        s[0] = '\0';
        for (int i = 0; w && w[i]; i++) {
            if (i) strcat(s, " ");
            strcat(s, w[i]);
        }
        return s;
    }
    case N_PIPE: {
        char *l = describe_node(node->u.binary.left);
        char *r = describe_node(node->u.binary.right);
        size_t len = (l ? strlen(l) : 0) + (r ? strlen(r) : 0) + 4;
        char *s = malloc(len);
        if (s) snprintf(s, len, "%s | %s", l ? l : "", r ? r : "");
        free(l);
        free(r);
        return s;
    }
    case N_REDIR:
        return describe_node(node->u.redir_node.body);
    default:
        return strdup("(compound command)");
    }
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
        /* Propagate flow control immediately */
        if (rc >= FLOW_BREAK) break;
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
        /* Propagate flow control immediately */
        if (rc >= FLOW_BREAK) break;
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
        /* Propagate flow control immediately (don't invert it!) */
        if (rc >= FLOW_BREAK) break;
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
        fflush(NULL);  /* Flush all stdio buffers before fork to avoid duplicate output */
        char *jobcmd = describe_node(node->u.binary.left);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            free(jobcmd);
            rc = 1;
            break;
        }
        if (pid == 0) {
            /* Own process group so the job can be signalled as a group and does
             * not receive the shell's terminal signals. setpgid is done in both
             * child and parent to avoid the classic race. */
            setpgid(0, 0);
            char ppid_buf[32];
            snprintf(ppid_buf, sizeof(ppid_buf), "%d", (int)getppid());
            vars_set(&sh->vars, "PPID", ppid_buf);
            int r = exec_node(sh, node->u.binary.left);
            fflush(NULL);
            _exit(r);
        }
        setpgid(pid, pid);
        sh->last_bg_pid = pid;
        job_t *j = job_register(&sh->jobs, pid, pid, jobcmd);
        free(jobcmd);
        /* Interactive shells announce the new job: `[1] 12345`. */
        if (sh->interactive && j)
            fprintf(stderr, "[%d] %ld\n", j->id, (long)pid);
        rc = 0;
        break;
    }

    case N_SUBSHELL: {
        fflush(NULL);  /* Flush all stdio buffers before fork to avoid duplicate output */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            rc = 1;
            break;
        }
        if (pid == 0) {
            /* Update PPID in child process */
            char ppid_buf[32];
            snprintf(ppid_buf, sizeof(ppid_buf), "%d", (int)getppid());
            vars_set(&sh->vars, "PPID", ppid_buf);

            /* Subshell: clear inherited traps (keep only traps set in this subshell) */
            for (int i = 0; i < NSIG; i++)
                sh->traps[i].set_in_this_shell = 0;

            /* Apply any redirections on the subshell node */
            redirect_ctx_t rctx;
            rctx.saved = NULL;
            rctx.error = 0;
            if (node->u.redir_node.redirs)
                redirect_apply(sh, node->u.redir_node.redirs, &rctx);
            int r = exec_node(sh, node->u.redir_node.body);
            /* FLOW_RETURN in subshell acts like exit with sh->last_exit */
            if (r == FLOW_RETURN) r = sh->last_exit;
            /* Normalize other flow control to 0 (break/continue outside loop) */
            if (r >= FLOW_BREAK) r = 0;
            sh->last_exit = r;
            /* POSIX: Fire EXIT trap only if it was set in this subshell */
            const char *exit_act = sh->traps[0].action;
            if (sh->traps[0].set_in_this_shell &&
                exit_act != SHELL_TRAP_DEFAULT && exit_act[0] != '\0') {
                sh->traps[0].action = SHELL_TRAP_DEFAULT;
                int exit_code = sh->last_exit;  /* Save exit code before trap */
                shell_run_string(sh, exit_act);
                /* Trap can modify $?, but subshell exits with original code */
                r = exit_code;
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
        /* If condition returns flow control (return/break/continue), propagate immediately */
        if (cond >= FLOW_BREAK) {
            rc = cond;
            break;
        }
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
        loop_scratch_t ls;
        loop_scratch_begin(sh, &ls);
        rc = 0;
        sh->loop_depth++;
        for (;;) {
            loop_scratch_iter(sh, &ls);
            int save_cond = sh->in_cond;
            sh->in_cond = 1;
            int cond = exec_node(sh, node->u.loop.cond);
            sh->in_cond = save_cond;
            /* Flow control from the CONDITION acts on THIS loop, exactly as it
             * does from the body: a single-level break/continue is absorbed here
             * (it must not leak past the loop), only a multi-level one or a
             * return propagates. `while case $# in (0) break;; esac; do ...` is
             * the standard "loop while args remain" idiom; propagating that
             * break raw broke the enclosing construct and hung modernish. */
            if (cond == FLOW_RETURN) { rc = cond; break; }
            if (cond == FLOW_BREAK) {
                if (sh->break_level > 0) { sh->break_level--; rc = cond; break; }
                rc = 0; break;
            }
            if (cond == FLOW_CONTINUE) {
                if (sh->break_level > 0) { sh->break_level--; rc = cond; break; }
                continue;   /* re-evaluate the condition */
            }
            if (cond != 0) break;
            rc = exec_node(sh, node->u.loop.body);
            /* `return` from the body must terminate the loop AND propagate up so
             * the enclosing function returns; without this the condition is just
             * re-evaluated and the body runs forever (an infinite loop for
             * `while true`, e.g. modernish's thisshellhas: return from a nested
             * case inside its arg-loop). break/continue are handled below. */
            if (rc == FLOW_RETURN) break;
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
        sh->loop_depth--;
        loop_scratch_end(sh, &ls);
        break;
    }

    case N_UNTIL: {
        loop_scratch_t ls;
        loop_scratch_begin(sh, &ls);
        rc = 0;
        sh->loop_depth++;
        for (;;) {
            loop_scratch_iter(sh, &ls);
            int save_cond = sh->in_cond;
            sh->in_cond = 1;
            int cond = exec_node(sh, node->u.loop.cond);
            sh->in_cond = save_cond;
            /* Flow control from the condition acts on THIS loop; a single-level
             * break/continue is absorbed, not propagated. See N_WHILE. */
            if (cond == FLOW_RETURN) { rc = cond; break; }
            if (cond == FLOW_BREAK) {
                if (sh->break_level > 0) { sh->break_level--; rc = cond; break; }
                rc = 0; break;
            }
            if (cond == FLOW_CONTINUE) {
                if (sh->break_level > 0) { sh->break_level--; rc = cond; break; }
                continue;   /* re-evaluate the condition */
            }
            if (cond == 0) break;
            rc = exec_node(sh, node->u.loop.body);
            /* `return` from the body must terminate the loop AND propagate up so
             * the enclosing function returns; without this the condition is just
             * re-evaluated and the body runs forever (an infinite loop for
             * `while true`, e.g. modernish's thisshellhas: return from a nested
             * case inside its arg-loop). break/continue are handled below. */
            if (rc == FLOW_RETURN) break;
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
        sh->loop_depth--;
        loop_scratch_end(sh, &ls);
        break;
    }

    case N_FOR: {
        char **wlist;
        /* Expanded BEFORE loop_scratch_begin, so the word list and the strings
         * it points at stay in the parent arena and survive every iteration's
         * reset. This is the allocation that makes a blanket per-iteration
         * reset of the shared scratch arena a use-after-free. */
        if (node->u.for_node.words) {
            wlist = expand_words(sh, node->u.for_node.words);
        } else {
            /* `for x; do` with no `in` clause: iterate $@.
             *
             * Copy the strings, not the pointers. POSIX iterates the list as it
             * was on entry, and `set --` in the body now frees the previous
             * list -- aliasing it would leave this walking freed memory from the
             * second iteration on. The copies live in the parent arena (this
             * runs before loop_scratch_begin), so they survive every iteration.
             */
            wlist = arena_alloc(sh->scratch,
                                (size_t)(sh->positional_n + 1) * sizeof(char *));
            for (int i = 0; i < sh->positional_n; i++)
                wlist[i] = arena_strdup(sh->scratch, sh->positional[i]);
            wlist[sh->positional_n] = NULL;
        }
        loop_scratch_t ls;
        loop_scratch_begin(sh, &ls);
        rc = 0;
        sh->loop_depth++;
        for (int i = 0; wlist && wlist[i]; i++) {
            loop_scratch_iter(sh, &ls);
            /* Check for readonly variable - exit loop immediately */
            if (vars_set(&sh->vars, node->u.for_node.var, wlist[i]) != 0) {
                rc = 1;
                break;
            }
            rc = exec_node(sh, node->u.for_node.body);
            /* `return` terminates the loop and propagates (see N_WHILE). Without
             * this a `for` ran its remaining iterations after a return -- masked
             * as a wrong result rather than a hang because the list is finite. */
            if (rc == FLOW_RETURN) break;
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
        sh->loop_depth--;
        loop_scratch_end(sh, &ls);
        break;
    }

    case N_CASE: {
        char *word = expand_word(sh, node->u.case_node.word);
        rc = 0;
        for (case_item_t *item = node->u.case_node.items; item; item = item->next) {
            int matched = 0;
            for (int pi = 0; item->patterns[pi]; pi++) {
                /* Quote-aware: a quoted metacharacter in the pattern must be
                 * literal (`case a in "*")` does not match), unlike expand_word,
                 * which quote-removes and lets it reach fnmatch as a wildcard. */
                char *pat = expand_word_pattern(sh, item->patterns[pi]);
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
    if (argc >= 2) {
        /* atoi() returned 0 for garbage, so `exit abc` exited 0 -- a failing
         * build step reporting success. POSIX: exit status is mod 256. */
        if (sh_parse_int(argv[1], INT_MIN, INT_MAX, &code) != 0) {
            fprintf(stderr, "silex: exit: %s: numeric argument required\n", argv[1]);
            code = 2;
        }
        code &= 0xff;
    }
    /* Fire EXIT trap (traps[0]) before exiting; clear first to prevent re-entry */
    const char *exit_action = sh->traps[0].action;
    if (exit_action != SHELL_TRAP_DEFAULT && exit_action[0] != '\0') {
        sh->traps[0].action = SHELL_TRAP_DEFAULT;
        shell_run_string(sh, exit_action);
        /* Trap can modify $?, but we exit with original code */
    }
    exit(code);
    return code; /* unreachable */
}

/* kill [-s signal | -signal | -signum] pid...   /   kill -l [status]
 *
 * Sends a signal (default TERM) to each target. Targets are numeric PIDs, a
 * process group (a negative number), or a job spec (%n/%+/%-/%string), which
 * signals the whole process group. Also covers what scripts and modernish need
 * (e.g. `kill -s PIPE "$$"` to probe SIGPIPE behaviour). */
static int exec_builtin_kill(shell_ctx_t *sh, int argc, char **argv)
{
    int sig = SIGTERM;
    int i = 1;

    if (i < argc && strcmp(argv[i], "-l") == 0) {
        /* -l [n]: with a number, print that signal's name (a status 128+sig from
         * a killed process is accepted too); with none, list all names. */
        if (i + 1 < argc) {
            int n = 0;
            if (sh_parse_int(argv[i + 1], 0, 255, &n) == 0) {
                if (n > 128) n -= 128;   /* exit status of a signal-killed process */
                const char *nm = signal_number_to_name(n);
                if (nm) { printf("%s\n", nm); return 0; }
                fprintf(stderr, "silex: kill: %d: invalid signal number\n", n);
                return 1;
            }
        }
        printf("HUP INT QUIT ILL TRAP ABRT BUS FPE KILL USR1 SEGV USR2 "
               "PIPE ALRM TERM CHLD CONT STOP TSTP TTIN TTOU\n");
        return 0;
    }

    /* Signal option. */
    if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *spec = NULL;
        if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "silex: kill: -s requires an argument\n");
                return 1;
            }
            spec = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
        } else {
            spec = argv[i] + 1;                 /* -TERM, -9, -SIGTERM */
        }
        if (spec) {
            int s;
            if (sh_parse_int(spec, 0, 64, &s) == 0) sig = s;
            else if ((s = signal_from_name(spec)) >= 0) sig = s;
            else {
                fprintf(stderr, "silex: kill: %s: invalid signal specification\n", spec);
                return 1;
            }
            i++;
        }
    }

    if (i >= argc) {
        fprintf(stderr, "silex: kill: usage: kill [-s sigspec | -signum] pid...\n");
        return 1;
    }

    int rc = 0;
    for (; i < argc; i++) {
        pid_t target;
        const char *arg = argv[i];
        if (!arg)
            break;                    /* argv is NULL-terminated at argc */
        if (arg[0] == '%') {
            /* Job spec: signal the whole process group (negative pgid). */
            job_t *j = parse_jobspec(sh, arg);
            if (!j) {
                fprintf(stderr, "silex: kill: %s: no such job\n", arg);
                rc = 1;
                continue;
            }
            target = -j->pgid;
        } else {
            int pid;
            if (sh_parse_int(arg, INT_MIN, INT_MAX, &pid) != 0) {
                fprintf(stderr, "silex: kill: %s: arguments must be process IDs or job specs\n", arg);
                rc = 1;
                continue;
            }
            target = (pid_t)pid;
        }
        if (kill(target, sig) != 0) {
            fprintf(stderr, "silex: kill: (%ld): %s\n", (long)target, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* Map a `set -o <name>` / `set +o <name>` long option name to its flag.
 * `value` is 1 for -o (enable), 0 for +o (disable). Returns 0 if recognised,
 * -1 otherwise. Long names are the POSIX/bash spellings of the short flags, so
 * that e.g. modernish's `set -o xtrace` works, not just `set -x`. */
static int set_option_byname(shell_ctx_t *sh, const char *name, int value)
{
    if      (strcmp(name, "errexit")  == 0) sh->opt_e = value;
    else if (strcmp(name, "nounset")  == 0) sh->opt_u = value;
    else if (strcmp(name, "xtrace")   == 0) sh->opt_x = value;
    else if (strcmp(name, "noglob")   == 0) sh->opt_f = value;
    else if (strcmp(name, "noexec")   == 0) sh->opt_n = value;
    else if (strcmp(name, "monitor")  == 0) sh->opt_m = value;
    else if (strcmp(name, "pipefail") == 0) sh->opt_pipefail = value;
    else return -1;
    return 0;
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
                case 'm': sh->opt_m = 1; break;
                case 'o':
                    if (i + 1 < argc) {
                        const char *opt = argv[i+1];
                        if (set_option_byname(sh, opt, 1) == 0) {
                            i++;
                        } else {
                            fprintf(stderr, "silex: set: %s: invalid option name\n", opt);
                            return 1;
                        }
                    } else {
                        fprintf(stderr, "silex: set: -o: option name required\n");
                        return 1;
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
                case 'm': sh->opt_m = 0; break;
                case 'o':
                    if (i + 1 < argc) {
                        const char *opt = argv[i+1];
                        if (set_option_byname(sh, opt, 0) == 0) {
                            i++;
                        } else {
                            fprintf(stderr, "silex: set: %s: invalid option name\n", opt);
                            return 1;
                        }
                    } else {
                        fprintf(stderr, "silex: set: +o: option name required\n");
                        return 1;
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

        /* Copy the strings rather than aliasing argv: argv points into the
         * scratch arena (exec_command expands into it), and a loop reclaims that
         * arena every iteration, so `while ...; do set -- $x; done` would leave
         * $1 dangling.
         *
         * Build the new list BEFORE releasing the old one. `set -- "$@"` expands
         * from the current positionals, so argv here can point straight into the
         * storage being replaced -- freeing first would read freed memory.
         */
        char **pos = malloc((size_t)(n + 1) * sizeof(char *));
        if (!pos) {
            fprintf(stderr, "silex: set: out of memory\n");
            return 1;
        }
        for (int j = 0; j < n; j++) {
            pos[j] = strdup(argv[i + j]);
            if (!pos[j]) {                       /* leave positionals untouched */
                while (j-- > 0) free(pos[j]);
                free(pos);
                fprintf(stderr, "silex: set: out of memory\n");
                return 1;
            }
        }
        pos[n] = NULL;

        /* Safe now: everything needed from the old list has been copied out.
         * positional_free() is a no-op unless a `set --` in THIS frame allocated
         * the list -- a function's arguments and the initial list belong to
         * someone else. Without this, `set -- "$@" "$x"` in a loop (the standard
         * POSIX way to build a list) re-copied every previous argument and kept
         * every generation: quadratic, and it died at ~3k items where dash is
         * fine. */
        positional_free(sh);
        sh->positional        = pos;
        sh->positional_n      = n;
        sh->positional_base   = pos;
        sh->positional_base_n = n;
    }
    return 0;
}

static int exec_builtin_export(shell_ctx_t *sh, int argc, char **argv)
{
    /* Handle export -p: print all exported variables */
    if (argc == 2 && strcmp(argv[1], "-p") == 0) {
        vars_print_exports(&sh->vars);
        return 0;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t nlen = (size_t)(eq - argv[i]);
            char *name  = strndup(argv[i], nlen);
            if (name) {
                if (vars_set_context(&sh->vars, name, eq + 1, "export") != 0) {
                    rc = 1;
                    /* POSIX: export is a special builtin; exit non-interactive shell on error
                     * EXCEPT when invoked via 'command' prefix */
                    if (!sh->interactive && !sh->in_command_builtin) {
                        free(name);
                        exit(1);
                    }
                } else {
                    vars_export(&sh->vars, name);
                }
                free(name);
            }
        } else {
            /* export NAME — mark existing variable for export */
            if (vars_get(&sh->vars, argv[i]) == NULL)
                vars_set(&sh->vars, argv[i], "");
            vars_export(&sh->vars, argv[i]);
        }
    }
    return rc;
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
        if (func_mode) {
            func_unregister(sh, argv[i]);
        } else {
            if (vars_unset_context(&sh->vars, argv[i], "unset") != 0) {
                rc = 1;
                /* POSIX: unset is a special builtin, so an error exits a
                 * non-interactive shell -- UNLESS invoked via `command`, which
                 * demotes it to a regular builtin (in_command_builtin). Without
                 * this guard, `command unset RO` on a readonly var killed the
                 * shell, failing modernish's FTL_CMDSPEXIT init check. Matches
                 * the export and readonly builtins above. */
                if (!sh->interactive && !sh->in_command_builtin) {
                    exit(1);
                }
            }
        }
    }
    return rc;
}

static int exec_builtin_readonly(shell_ctx_t *sh, int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t nlen = (size_t)(eq - argv[i]);
            char *name  = strndup(argv[i], nlen);
            if (name) {
                if (vars_set_context(&sh->vars, name, eq + 1, "readonly") != 0) {
                    rc = 1;
                    /* POSIX: readonly is a special builtin; exit non-interactive shell on error
                     * EXCEPT when invoked via 'command' prefix */
                    if (!sh->interactive && !sh->in_command_builtin) {
                        free(name);
                        exit(1);
                    }
                } else {
                    vars_readonly(&sh->vars, name);
                }
                free(name);
            }
        } else {
            vars_readonly(&sh->vars, argv[i]);
        }
    }
    return rc;
}

static int exec_builtin_cd(shell_ctx_t *sh, int argc, char **argv)
{
    /* POSIX: cd [-L|-P] [directory | -]
     *
     * `--` (end of options) was not handled at all, so `cd -- "$dir"` tried to
     * chdir("--") and failed with "--: No such file or directory". That is the
     * idiom for a directory that might begin with a dash, and modernish's
     * bootstrap uses it -- so modernish could not start.
     *
     * -L/-P are accepted; silex resolves physically either way (see below).
     */
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "-P") == 0) continue;
        break;                                  /* not an option */
    }

    const char *dir;
    int print_dir = 0;

    if (i >= argc) {
        dir = vars_get(&sh->vars, "HOME");
        if (!dir || !*dir) {
            fprintf(stderr, "silex: cd: HOME not set\n");
            return 1;
        }
    } else if (strcmp(argv[i], "-") == 0) {
        /* cd - : switch to OLDPWD and echo it, per POSIX. */
        dir = vars_get(&sh->vars, "OLDPWD");
        if (!dir || !*dir) {
            fprintf(stderr, "silex: cd: OLDPWD not set\n");
            return 1;
        }
        print_dir = 1;
    } else {
        dir = argv[i];
    }

    /* Remember where we were before moving, so `cd -` works next time. */
    char oldpwd[PATH_MAX];
    const char *prev = getcwd(oldpwd, sizeof(oldpwd)) ? oldpwd : NULL;

    if (chdir(dir) != 0) {
        fprintf(stderr, "silex: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }

    if (prev)
        vars_set(&sh->vars, "OLDPWD", prev);

    char pwd[PATH_MAX];
    if (getcwd(pwd, sizeof(pwd))) {
        vars_set(&sh->vars, "PWD", pwd);
        if (print_dir)
            printf("%s\n", pwd);
    }

    return 0;
}

static int exec_builtin_shift(shell_ctx_t *sh, int argc, char **argv)
{
    int n = 1;
    if (argc >= 2 && sh_parse_int(argv[1], 0, INT_MAX, &n) != 0) {
        fprintf(stderr, "silex: shift: %s: numeric argument required\n", argv[1]);
        return 1;
    }
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
    /* trap with no arguments: print all traps */
    if (argc < 2) {
        for (int sig = 0; sig < NSIG; sig++) {
            const char *act = sh->traps[sig].action;
            if (act == SHELL_TRAP_DEFAULT) continue;

            const char *signame = NULL;
            if (sig == 0) signame = "EXIT";
            else if (sig == SIGINT) signame = "INT";
            else if (sig == SIGTERM) signame = "TERM";
            else if (sig == SIGHUP) signame = "HUP";
            else if (sig == SIGQUIT) signame = "QUIT";
            else if (sig == SIGPIPE) signame = "PIPE";
            else if (sig == SIGCHLD) signame = "CHLD";
            else if (sig == SIGUSR1) signame = "USR1";
            else if (sig == SIGUSR2) signame = "USR2";
            else continue;

            if (act[0] == '\0') {
                /* SHELL_TRAP_IGNORE: empty string means ignore */
                printf("trap -- '' %s\n", signame);
            } else {
                /* Print the action with proper quoting */
                printf("trap -- '%s' %s\n", act, signame);
            }
        }
        return 0;
    }

    const char *action = argv[1];
    /* POSIX: trap action is stored as-is and expanded when trap fires */
    const char *trap_action = action;

    for (int i = 2; i < argc; i++) {
        /* A signal number must be a valid signal, not whatever atoi() made of
         * the string. Fall through to the name lookup when it is not numeric. */
        int sig = 0;
        if (sh_parse_int(argv[i], 0, 64, &sig) != 0)
            sig = -1;
        if (sig < 0) {
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
            sh->traps[sig].set_in_this_shell = 1;
        } else if (strcmp(action, "") == 0) {
            sh->traps[sig].action = SHELL_TRAP_IGNORE;
            sh->traps[sig].set_in_this_shell = 1;
        } else {
            sh->traps[sig].action = arena_strdup(&sh->parse_arena, trap_action);
            sh->traps[sig].set_in_this_shell = 1;
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
    /* read [-r] VAR...
     * NB: not `optind` -- that is getopt's global in <unistd.h>, and clang's
     * -Wshadow (unlike gcc's) rejects shadowing it. */
    int raw = 0;
    int opt_i = 1;
    while (opt_i < argc && argv[opt_i][0] == '-') {
        if (strcmp(argv[opt_i], "-r") == 0) { raw = 1; opt_i++; }
        else if (strcmp(argv[opt_i], "--") == 0) { opt_i++; break; }
        else break;
    }

    strbuf_t line;
    sb_init(&line, 256);

    int c;
    /* EOF is signalled to the post-loop code via `c` (see the returns below),
     * not a flag; the loop just reads until newline or EOF. */
    while (1) {
        c = fgetc(stdin);
        if (c == EOF) break;
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

    int nvars = argc - opt_i;
    if (nvars == 0) {
        /* No variable names: discard */
        sb_free(&line);
        return (c == EOF && sb_len(&line) == 0) ? 1 : 0;
    }

    char *linecopy = strdup(sb_str(&line));
    sb_free(&line);
    if (!linecopy) return 1;

    char *p = linecopy;
    for (int vi = opt_i; vi < argc; vi++) {
        /* Trim leading IFS on all but last */
        if (vi < argc - 1) {
            while (*p && strchr(ifs, (unsigned char)*p)) p++;
        }
        if (vi == argc - 1) {
            /* Last var gets rest of line (stripped of leading IFS if >1 var) */
            if (argc - opt_i > 1) {
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

    /* POSIX: repeat format until all arguments consumed */
    do {
        int consumed_arg = 0;
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
                    if (argi < argc) {
                        fputs(argv[argi++], stdout);
                        consumed_arg = 1;
                    }
                    break;
                case 'b': {
                    /* %b - interpret backslash escapes */
                    if (argi < argc) {
                        const char *s = argv[argi++];
                        consumed_arg = 1;
                        while (*s) {
                            if (*s == '\\' && s[1]) {
                                s++;
                                switch (*s) {
                                case 'n':  putchar('\n'); break;
                                case 't':  putchar('\t'); break;
                                case 'r':  putchar('\r'); break;
                                case 'b':  putchar('\b'); break;
                                case 'f':  putchar('\f'); break;
                                case 'v':  putchar('\v'); break;
                                case '\\': putchar('\\'); break;
                                case '0': case '1': case '2': case '3':
                                case '4': case '5': case '6': case '7': {
                                    unsigned char oc = *s - '0';
                                    if (s[1] >= '0' && s[1] <= '7') {
                                        oc = (unsigned char)(oc * 8 + (s[1] - '0'));
                                        s++;
                                        if (s[1] >= '0' && s[1] <= '7') {
                                            oc = (unsigned char)(oc * 8 + (s[1] - '0'));
                                            s++;
                                        }
                                    }
                                    putchar((int)oc);
                                    break;
                                }
                                case 'c':
                                    /* %b \c - terminate output */
                                    return 0;
                                default:
                                    putchar('\\');
                                    putchar(*s);
                                    break;
                                }
                                s++;
                            } else {
                                putchar(*s++);
                            }
                        }
                    }
                    break;
                }
                case 'd': {
                    long v = argi < argc ? atol(argv[argi++]) : 0;
                    if (argi > 2) consumed_arg = 1;
                    printf("%ld", v);
                    break;
                }
                case 'f': {
                    double v = argi < argc ? atof(argv[argi++]) : 0.0;
                    if (argi > 2) consumed_arg = 1;
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

        /* Repeat if there are more arguments and we consumed at least one this pass */
        if (!consumed_arg) break;
    } while (argi < argc);

    /* Check for write errors */
    if (fflush(stdout) != 0 || ferror(stdout))
        return 1;

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
        if (strcmp(op, "-L") == 0) { struct stat st; return (lstat(a, &st) == 0 && S_ISLNK(st.st_mode)) ? 0 : 1; }
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
            int a_exists = (stat(a, &sa) == 0);
            int b_exists = (stat(b, &sb2) == 0);
            if (!a_exists) return 1;
            if (!b_exists) return 0;
            return (sa.st_mtime > sb2.st_mtime) ? 0 : 1;
        }
        if (strcmp(op, "-ot") == 0) {
            struct stat sa, sb2;
            int a_exists = (stat(a, &sa) == 0);
            int b_exists = (stat(b, &sb2) == 0);
            if (!b_exists) return 1;
            if (!a_exists) return 0;
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
    if (argc >= 2) {
        if (sh_parse_int(argv[1], INT_MIN, INT_MAX, &code) != 0) {
            fprintf(stderr, "silex: return: %s: numeric argument required\n", argv[1]);
            code = 2;
        }
        code &= 0xff;
    }
    sh->last_exit = code;
    return FLOW_RETURN;
}

/* Resolve a job spec ("%1", "%+", "%%", "%-", "%str", "%?str") to a job, or
 * NULL with a message. Shared by jobs/fg/bg/kill/wait. A bare "%" means %+. */
static job_t *parse_jobspec(shell_ctx_t *sh, const char *spec)
{
    if (spec[0] != '%') return NULL;
    const char *s = spec + 1;

    if (*s == '\0' || strcmp(s, "+") == 0 || strcmp(s, "%") == 0)
        return job_current(&sh->jobs);
    if (strcmp(s, "-") == 0)
        return job_previous(&sh->jobs);

    if (*s >= '0' && *s <= '9') {
        int id = 0;
        if (sh_parse_int(s, 1, INT_MAX, &id) != 0) return NULL;
        return job_find_by_id(&sh->jobs, id);
    }

    /* %?str: command contains str; %str: command starts with str. */
    int substring = (*s == '?');
    if (substring) s++;
    for (job_t *j = sh->jobs.head; j; j = j->next) {
        if (!j->command) continue;
        if (substring ? (strstr(j->command, s) != NULL)
                      : (strncmp(j->command, s, strlen(s)) == 0))
            return j;
    }
    return NULL;
}

/* Map a wait/kill argument to a pid: a job spec resolves via the job table,
 * otherwise it is parsed as a numeric pid. Returns -1 on error. */
static pid_t jobspec_or_pid(shell_ctx_t *sh, const char *arg)
{
    if (arg[0] == '%') {
        job_t *j = parse_jobspec(sh, arg);
        return j ? j->pid : (pid_t)-1;
    }
    int pid;
    if (sh_parse_int(arg, 0, INT_MAX, &pid) != 0)
        return (pid_t)-1;
    return (pid_t)pid;
}

static int exec_builtin_wait(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc >= 2) {
        pid_t pid = jobspec_or_pid(sh, argv[1]);
        if (pid == (pid_t)-1) {
            fprintf(stderr, "silex: wait: %s: no such job\n", argv[1]);
            return 127;
        }
        int rc = job_wait(&sh->jobs, pid);
        job_t *j = job_find_by_pid(&sh->jobs, pid);
        if (j && j->state == JOB_DONE) job_remove(&sh->jobs, j);
        return rc;
    }
    /* Wait for all background jobs. */
    int rc = 0;
    job_t *j = sh->jobs.head;
    while (j) {
        job_t *next = j->next;
        if (j->state == JOB_RUNNING)
            rc = job_wait(&sh->jobs, j->pid);
        if (j->state == JOB_DONE)
            job_remove(&sh->jobs, j);
        j = next;
    }
    return rc;
}

/* jobs [-l|-p]: list jobs. -l adds the pgid, -p prints only pids. */
static int exec_builtin_jobs(shell_ctx_t *sh, int argc, char **argv)
{
    int show_pid_only = 0, show_pgid = 0;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-p") == 0) show_pid_only = 1;
        else if (strcmp(argv[i], "-l") == 0) show_pgid = 1;
        else if (strcmp(argv[i], "--") == 0) break;
    }
    job_reap(&sh->jobs);
    job_t *j = sh->jobs.head;
    while (j) {
        job_t *next = j->next;
        if (show_pid_only) {
            printf("%ld\n", (long)j->pid);
        } else {
            char cur = (j->id == sh->jobs.current) ? '+'
                     : (j->id == sh->jobs.previous) ? '-' : ' ';
            const char *st = j->state == JOB_RUNNING ? "Running"
                           : j->state == JOB_STOPPED ? "Stopped" : "Done";
            if (show_pgid)
                printf("[%d]%c %ld %-8s %s\n", j->id, cur, (long)j->pgid, st,
                       j->command ? j->command : "");
            else
                printf("[%d]%c %-8s %s\n", j->id, cur, st,
                       j->command ? j->command : "");
        }
        /* A reported Done job leaves the table after `jobs` displays it once. */
        if (j->state == JOB_DONE)
            job_remove(&sh->jobs, j);
        j = next;
    }
    return 0;
}

/* Bring a job to the foreground (fg) or resume it in the background (bg).
 * Without a controlling-terminal / monitor-mode layer, fg simply resumes (if
 * stopped) and waits; bg resumes a stopped job and returns. */
static int exec_builtin_fgbg(shell_ctx_t *sh, int argc, char **argv, int foreground)
{
    const char *name = foreground ? "fg" : "bg";
    /* Consume any pending stopped/continued notifications first, so the job's
     * recorded state is current before we decide whether to SIGCONT it. */
    job_reap(&sh->jobs);
    job_t *j;
    if (argc >= 2) {
        j = parse_jobspec(sh, argv[1]);
        if (!j) {
            fprintf(stderr, "silex: %s: %s: no such job\n", name, argv[1]);
            return 1;
        }
    } else {
        j = job_current(&sh->jobs);
        if (!j) {
            fprintf(stderr, "silex: %s: no current job\n", name);
            return 1;
        }
    }

    if (j->command)
        fprintf(stderr, "%s\n", j->command);

    if (foreground && sh->job_control && sh->tty_fd >= 0)
        tcsetpgrp(sh->tty_fd, j->pgid);   /* hand the terminal to the job first */

    if (j->state == JOB_STOPPED) {
        if (kill(-j->pgid, SIGCONT) != 0 && kill(j->pid, SIGCONT) != 0)
            perror("silex: kill (SIGCONT)");
        j->state = JOB_RUNNING;
    }

    if (foreground) {
        /* Wait with WUNTRACED under job control so the job can be stopped again
         * (^Z), then reclaim the terminal for the shell. */
        pid_t pid = j->pid;
        int status = 0;
        while (waitpid(pid, &status, sh->job_control ? WUNTRACED : 0) < 0 && errno == EINTR) {}
        if (sh->job_control && sh->tty_fd >= 0)
            tcsetpgrp(sh->tty_fd, sh->shell_pgid);
        job_t *k = job_find_by_pid(&sh->jobs, pid);
        if (WIFSTOPPED(status)) {
            if (k) { k->state = JOB_STOPPED; k->status = status; }
            fprintf(stderr, "\n[%d]+ Stopped  %s\n", k ? k->id : 0,
                    (k && k->command) ? k->command : "");
            return 128 + WSTOPSIG(status);
        }
        if (k) { k->status = status; k->state = JOB_DONE; job_remove(&sh->jobs, k); }
        if (WIFEXITED(status))   return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 1;
    }
    return 0;
}

static int exec_builtin_fg(shell_ctx_t *sh, int argc, char **argv)
{
    return exec_builtin_fgbg(sh, argc, argv, 1);
}

static int exec_builtin_bg(shell_ctx_t *sh, int argc, char **argv)
{
    return exec_builtin_fgbg(sh, argc, argv, 0);
}

static int exec_builtin_times(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;
    (void)argc;
    (void)argv;

    struct tms buf;
    if (times(&buf) == (clock_t)-1) {
        fprintf(stderr, "silex: times: error getting process times\n");
        return 1;
    }

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;  /* Fallback */

    /* Format: shell user time, shell system time */
    long shell_user_min = buf.tms_utime / clk_tck / 60;
    long shell_user_sec = (buf.tms_utime / clk_tck) % 60;
    long shell_user_ms = (buf.tms_utime % clk_tck) * 1000 / clk_tck;

    long shell_sys_min = buf.tms_stime / clk_tck / 60;
    long shell_sys_sec = (buf.tms_stime / clk_tck) % 60;
    long shell_sys_ms = (buf.tms_stime % clk_tck) * 1000 / clk_tck;

    /* Output to stdout and check for errors */
    if (printf("%ldm%ld.%03lds %ldm%ld.%03lds\n",
               shell_user_min, shell_user_sec, shell_user_ms,
               shell_sys_min, shell_sys_sec, shell_sys_ms) < 0) {
        fprintf(stderr, "silex: times: I/O error\n");
        return 2;
    }

    /* Format: children user time, children system time */
    long child_user_min = buf.tms_cutime / clk_tck / 60;
    long child_user_sec = (buf.tms_cutime / clk_tck) % 60;
    long child_user_ms = (buf.tms_cutime % clk_tck) * 1000 / clk_tck;

    long child_sys_min = buf.tms_cstime / clk_tck / 60;
    long child_sys_sec = (buf.tms_cstime / clk_tck) % 60;
    long child_sys_ms = (buf.tms_cstime % clk_tck) * 1000 / clk_tck;

    if (printf("%ldm%ld.%03lds %ldm%ld.%03lds\n",
               child_user_min, child_user_sec, child_user_ms,
               child_sys_min, child_sys_sec, child_sys_ms) < 0) {
        fprintf(stderr, "silex: times: I/O error\n");
        return 2;
    }

    return 0;
}

static int exec_builtin_source(shell_ctx_t *sh, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "silex: .: filename argument required\n");
        return 1;
    }

    const char *filename = argv[1];
    char resolved_path[PATH_MAX];
    const char *actual_path = filename;

    /* If filename contains '/', use it as-is; otherwise search PATH */
    if (!strchr(filename, '/')) {
        /* Search PATH for the file */
        const char *pathval = vars_get(&sh->vars, "PATH");
        if (pathval && *pathval) {
            char *pathcopy = strdup(pathval);
            if (pathcopy) {
                char *saveptr = NULL;
                char *dir = strtok_r(pathcopy, ":", &saveptr);
                int found = 0;
                while (dir) {
                    int n = snprintf(resolved_path, sizeof(resolved_path), "%s/%s", dir, filename);
                    if (n > 0 && (size_t)n < sizeof(resolved_path)) {
                        struct stat st;
                        /* Check if file exists, is regular, and is readable */
                        if (stat(resolved_path, &st) == 0 && S_ISREG(st.st_mode) &&
                            access(resolved_path, R_OK) == 0) {
                            actual_path = resolved_path;
                            found = 1;
                            break;
                        }
                    }
                    dir = strtok_r(NULL, ":", &saveptr);
                }
                free(pathcopy);
                if (!found) {
                    /* File not found in PATH */
                    const char *cmd = strcmp(argv[0], ".") == 0 ? "." : "source";
                    fprintf(stderr, "%s: %s: not found\n", cmd, filename);
                    sh->last_exit = 1;
                    /* POSIX: source/. is a special builtin; exit non-interactive shell on error */
                    if (!sh->interactive) {
                        exit(1);
                    }
                    return 1;
                }
            }
        } else {
            /* No PATH set */
            const char *cmd = strcmp(argv[0], ".") == 0 ? "." : "source";
            fprintf(stderr, "%s: %s: not found\n", cmd, filename);
            sh->last_exit = 1;
            /* POSIX: source/. is a special builtin; exit non-interactive shell on error */
            if (!sh->interactive) {
                exit(1);
            }
            return 1;
        }
    }

    /* Check if file exists and is readable */
    FILE *fp = fopen(actual_path, "r");
    if (!fp) {
        /* Print error with proper command name */
        const char *cmd = strcmp(argv[0], ".") == 0 ? "." : "source";
        fprintf(stderr, "%s: %s: ", cmd, actual_path);

        /* Customize error message based on errno */
        if (errno == ENOENT) {
            fprintf(stderr, "not found\n");
        } else if (errno == EACCES) {
            fprintf(stderr, "Permission denied\n");
        } else {
            perror("");
        }
        sh->last_exit = 1;
        /* POSIX: source/. is a special builtin; exit non-interactive shell on error */
        if (!sh->interactive) {
            exit(1);
        }
        return 1;
    }
    fclose(fp);

    return shell_run_file(sh, actual_path);
}

static int exec_builtin_break(shell_ctx_t *sh, int argc, char **argv)
{
    int n = 1;
    if (argc >= 2 && sh_parse_int(argv[1], 1, INT_MAX, &n) != 0) {
        fprintf(stderr, "silex: break: %s: numeric argument required\n", argv[1]);
        return 1;
    }
    if (n < 1) n = 1;
    /* POSIX: break outside of a loop should be silently ignored */
    if (sh->loop_depth == 0) return 0;
    sh->break_level = n - 1;  /* loops decrement; propagate if > 0 */
    return FLOW_BREAK;
}

static int exec_builtin_continue(shell_ctx_t *sh, int argc, char **argv)
{
    int n = 1;
    if (argc >= 2 && sh_parse_int(argv[1], 1, INT_MAX, &n) != 0) {
        fprintf(stderr, "silex: continue: %s: numeric argument required\n", argv[1]);
        return 1;
    }
    if (n < 1) n = 1;
    /* POSIX: continue outside of a loop should be silently ignored */
    if (sh->loop_depth == 0) return 0;
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
        /* Check for write errors */
        if (fflush(stdout) != 0 || ferror(stdout))
            return 1;
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
        /* A name containing a slash is a pathname, not a command to look up:
         * POSIX says `command -v` reports it verbatim if it is an executable
         * file, else fails. Without this, `command -v /bin/sh` returned failure,
         * so modernish's shell probe -- which guards each candidate with
         * `command -v "$shell"` on an absolute path -- skipped every candidate
         * and could not find a usable shell. */
        if (strchr(name, '/')) {
            if (access(name, X_OK) == 0) {
                if (verbose) printf("%s is %s\n", name, name);
                else         printf("%s\n", name);
                return 0;
            }
            return 127;
        }

        /* Check shell keywords first */
        const char *keywords[] = {
            "!", "{", "}", "case", "do", "done", "elif", "else", "esac",
            "fi", "for", "if", "in", "then", "until", "while", NULL
        };
        for (const char **kw = keywords; *kw; kw++) {
            if (strcmp(name, *kw) == 0) {
                if (verbose) printf("%s is a shell keyword\n", name);
                else         printf("%s\n", name);
                return 0;
            }
        }
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
        /* Not found: return 1 without printing error (exit code indicates failure) */
        return 1;
    }

    /* Execute name bypassing shell functions (builtins still apply, but without special builtin semantics) */
    int old_in_command = sh->in_command_builtin;
    sh->in_command_builtin = 1;
    int rc = exec_simple_cmd(sh, argv + i, NULL, NULL);
    sh->in_command_builtin = old_in_command;
    return rc;
}

static int exec_builtin_type(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        /* Check shell keywords first */
        const char *keywords[] = {
            "!", "{", "}", "case", "do", "done", "elif", "else", "esac",
            "fi", "for", "if", "in", "then", "until", "while", NULL
        };
        int is_keyword = 0;
        for (const char **kw = keywords; *kw; kw++) {
            if (strcmp(name, *kw) == 0) {
                printf("%s is a shell keyword\n", name);
                is_keyword = 1;
                break;
            }
        }
        if (is_keyword) continue;
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
    /* OPTIND is user-settable (OPTIND=abc), so it is untrusted. */
    int opt_i = 1;  /* not `opt_i`: getopt's global (clang -Wshadow) */
    if (optind_str && sh_parse_int(optind_str, 1, INT_MAX, &opt_i) != 0)
        opt_i = 1;
    if (opt_i < 1) opt_i = 1;

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
    int optpos = 1;
    if (pos_str && sh_parse_int(pos_str, 1, INT_MAX, &optpos) != 0)
        optpos = 1;
    if (optpos < 1) optpos = 1;

    /* Check bounds */
    if (opt_i > nargs) {
        vars_set(&sh->vars, varname, "?");
        return 1;
    }
    const char *arg = args[opt_i - 1];

    /* Check if arg is an option */
    if (arg[0] != '-' || arg[1] == '\0') {
        vars_set(&sh->vars, varname, "?");
        return 1;
    }
    if (strcmp(arg, "--") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", opt_i + 1);
        vars_set(&sh->vars, "OPTIND", buf);
        vars_set(&sh->vars, "__OPTPOS", "1");
        vars_set(&sh->vars, varname, "?");
        return 1;
    }

    /* Get option char at optpos within arg */
    if ((size_t)optpos >= strlen(arg)) {
        /* Move to next arg */
        opt_i++;
        optpos = 1;
        if (opt_i > nargs) {
            vars_set(&sh->vars, varname, "?");
            return 1;
        }
        arg = args[opt_i - 1];
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
                opt_i++;
                optpos = 1;
            } else {
                opt_i++;
                optpos = 1;
                if (opt_i > nargs) {
                    if (!silent) fprintf(stderr, "silex: getopts: option requires an argument -- %c\n", opt);
                    vars_set(&sh->vars, varname, silent ? ":" : "?");
                    vars_set(&sh->vars, "OPTARG", opt_str);
                } else {
                    vars_set(&sh->vars, "OPTARG", args[opt_i - 1]);
                    opt_i++;
                }
            }
        } else {
            vars_set(&sh->vars, "OPTARG", "");
            optpos++;
            if ((size_t)optpos >= strlen(arg)) {
                opt_i++;
                optpos = 1;
            }
        }
    }

    /* Update OPTIND and __OPTPOS */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", opt_i);
    vars_set(&sh->vars, "OPTIND", buf);
    snprintf(buf, sizeof(buf), "%d", optpos);
    vars_set(&sh->vars, "__OPTPOS", buf);

    return 0;
}

/* -------------------------------------------------------------------------
 * alias [name[=value] ...]
 * Without arguments: list all aliases
 * With name=value: set alias
 * With name only: print that alias
 * ------------------------------------------------------------------------- */

static int exec_builtin_alias(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;

    if (argc == 1) {
        /* List all aliases */
        for (int i = 0; i < 256; i++) {
            alias_entry_t *e = sh->aliases[i];
            while (e) {
                printf("alias %s='%s'\n", e->name, e->value);
                e = e->next;
            }
        }
        return 0;
    }

    /* Process each argument */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *eq = strchr(arg, '=');

        if (eq) {
            /* Set alias: name=value */
            size_t nlen = (size_t)(eq - arg);
            char *name = strndup(arg, nlen);
            const char *value = eq + 1;
            alias_register(sh, name, value);
            free(name);
        } else {
            /* Print specific alias */
            const char *value = alias_lookup(sh, arg);
            if (value) {
                printf("alias %s='%s'\n", arg, value);
            } else {
                fprintf(stderr, "alias: %s: not found\n", arg);
                return 1;
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * unalias [-a] name [name ...]
 * Remove alias definitions
 * -a: remove all aliases
 * ------------------------------------------------------------------------- */

static int exec_builtin_unalias(shell_ctx_t *sh, int argc, char **argv)
{
    (void)sh;

    if (argc < 2) {
        fprintf(stderr, "unalias: usage: unalias [-a] name [name ...]\n");
        return 2;
    }

    int i = 1;
    if (strcmp(argv[i], "-a") == 0) {
        /* Remove all aliases */
        alias_clear_all(sh);
        return 0;
    }

    /* Remove specified aliases */
    int rc = 0;
    for (; i < argc; i++) {
        const char *name = argv[i];
        if (alias_lookup(sh, name)) {
            alias_unregister(sh, name);
        } else {
            fprintf(stderr, "unalias: %s: not found\n", name);
            rc = 1;
        }
    }

    return rc;
}

static int exec_builtin_hash(shell_ctx_t *sh, int argc, char **argv)
{
    /* hash [-r] [name ...] */
    int i = 1;
    int was_r_flag = 0;

    /* Check for -r flag (clear cache) */
    if (i < argc && strcmp(argv[i], "-r") == 0) {
        path_cache_clear(sh);
        was_r_flag = 1;
        i++;
    }

    /* If no arguments (or only -r flag), print/clear and exit */
    if (i >= argc) {
        /* If no -r flag was given, print the hash table */
        if (!was_r_flag) {
            /* Display cached entries */
            for (int j = 0; j < 256; j++) {
                path_cache_entry_t *e = sh->path_cache[j];
                while (e) {
                    if (e->found && e->path) {
                        printf("%s\n", e->path);
                    }
                    e = e->next;
                }
            }
        }
        return 0;
    }

    /* Cache specified commands */
    int rc = 0;
    for (; i < argc; i++) {
        const char *name = argv[i];

        /* Skip builtins and commands with / */
        if (strchr(name, '/') || find_shell_builtin(name) || find_applet(name)) {
            continue;
        }

        /* Resolve the command in PATH */
        char resolved[PATH_MAX];
        if (path_resolve(sh, name, resolved, sizeof(resolved))) {
            path_cache_put(sh, name, resolved);
        } else {
            fprintf(stderr, "hash: %s: not found\n", name);
            rc = 1;
        }
    }

    return rc;
}

