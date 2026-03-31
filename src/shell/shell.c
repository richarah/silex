/* shell.c — shell initialization, main loop, and cleanup */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "shell.h"
#include "exec.h"
#include "expand.h"
#include "../util/arena.h"
#include "../util/strbuf.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * SIGPIPE: the shell ignores SIGPIPE so broken-pipe errors in builtins
 * produce EPIPE from write() rather than killing the process.
 * External commands restore SIG_DFL before execvp (see exec.c).
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Trap signal handling
 * Simple global used only to record which shell_ctx to call traps on.
 * This is the one permitted piece of global mutable state (signal delivery).
 * ------------------------------------------------------------------------- */

static shell_ctx_t *g_trap_shell = NULL;

void shell_signal_handler(int sig)
{
    if (!g_trap_shell) return;
    if (sig < 0 || sig >= NSIG) return;

    const char *action = g_trap_shell->traps[sig].action;
    if (!action) return;                  /* SIG_DFL */
    if (action[0] == '\0') return;        /* SIG_IGN (SHELL_TRAP_IGNORE) */

    /* Run the trap action string */
    shell_run_string(g_trap_shell, action);
}

/* -------------------------------------------------------------------------
 * errexit check helper: consumes the and_or_exempt flag and tests
 * whether a non-zero rc from exec_node should trigger -e exit.
 * Returns 1 if we should stop, 0 if we should continue.
 * ------------------------------------------------------------------------- */
static int errexit_should_stop(shell_ctx_t *sh, int rc)
{
    int exempt = sh->and_or_exempt;
    sh->and_or_exempt = 0;
    return sh->opt_e && rc != 0 && !sh->in_cond && !exempt;
}

/* -------------------------------------------------------------------------
 * shell_init
 * ------------------------------------------------------------------------- */

int shell_init(shell_ctx_t *sh, int argc, char **argv)
{
    memset(sh, 0, sizeof(*sh));

    arena_init(&sh->parse_arena);
    arena_init(&sh->scratch_arena);
    vars_init(&sh->vars, &sh->parse_arena);
    job_list_init(&sh->jobs);

    sh->last_exit    = 0;
    sh->last_bg_pid  = 0;
    sh->opt_e        = 0;
    sh->opt_u        = 0;
    sh->opt_x        = 0;
    sh->opt_f        = 0;
    sh->opt_pipefail = 0;
    sh->opt_n        = 0;

    /* $0 */
    if (argc > 0 && argv && argv[0])
        sh->script_name = arena_strdup(&sh->parse_arena, argv[0]);
    else
        sh->script_name = arena_strdup(&sh->parse_arena, "silex");

    /* $1..$N */
    if (argc > 1 && argv) {
        sh->positional_n = argc - 1;
        sh->positional   = arena_alloc(&sh->parse_arena,
                               (size_t)(argc) * sizeof(char *));
        for (int i = 1; i < argc; i++)
            sh->positional[i - 1] = arena_strdup(&sh->parse_arena, argv[i]);
        sh->positional[argc - 1] = NULL;
    } else {
        sh->positional_n = 0;
        sh->positional   = arena_alloc(&sh->parse_arena, sizeof(char *));
        sh->positional[0] = NULL;
    }

    /* Import all environment variables (as exported shell variables) */
    vars_import_env(&sh->vars);

    /* Set default shell variables */
    const char *path = getenv("PATH");
    if (path)
        vars_set(&sh->vars, "PATH", path);
    else
        vars_set(&sh->vars, "PATH", "/usr/local/bin:/usr/bin:/bin");

    vars_set(&sh->vars, "IFS", " \t\n");

    const char *home = getenv("HOME");
    if (home) vars_set(&sh->vars, "HOME", home);

    const char *user = getenv("USER");
    if (user) vars_set(&sh->vars, "USER", user);

    const char *pwd = getenv("PWD");
    if (pwd) vars_set(&sh->vars, "PWD", pwd);

    /* Initialise all traps to default */
    for (int i = 0; i < NSIG; i++)
        sh->traps[i].action = SHELL_TRAP_DEFAULT;

    /* SILEX_TRACE: tracing/debugging mode */
    const char *trace_env = getenv("SILEX_TRACE");
    if (trace_env) {
        sh->trace_level = atoi(trace_env);
        if (sh->trace_level < 0) sh->trace_level = 0;
        if (sh->trace_level > 2) sh->trace_level = 2;
    }

    /* Ignore SIGPIPE in the shell process; children restore SIG_DFL before exec */
    signal(SIGPIPE, SIG_IGN);

    /* Register as the global trap shell */
    g_trap_shell = sh;

    return 0;
}

/* -------------------------------------------------------------------------
 * shell_run_string
 * ------------------------------------------------------------------------- */

int shell_run_string(shell_ctx_t *sh, const char *script)
{
    if (!script) return 0;

    lexer_t  lex;
    parser_t par;

    lexer_init_str(&lex, script, &sh->parse_arena);
    parser_init(&par, &lex, &sh->parse_arena);

    int rc = 0;
    for (;;) {
        node_t *node = parser_parse(&par);
        if (!node) break;
        if (par.error) break;

        if (!sh->opt_n) {
            rc = exec_node(sh, node);
            sh->last_exit = rc;
            /* Reclaim scratch expansion memory after each top-level command */
            arena_reset(&sh->scratch_arena);
            if (errexit_should_stop(sh, rc))
                break;
        }
    }

    lexer_free(&lex);
    return sh->last_exit;
}

/* -------------------------------------------------------------------------
 * shell_run_file
 * ------------------------------------------------------------------------- */

int shell_run_file(shell_ctx_t *sh, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        sh->last_exit = 1;
        return 1;
    }

    lexer_t  lex;
    parser_t par;

    lexer_init_fp(&lex, fp, &sh->parse_arena);
    parser_init(&par, &lex, &sh->parse_arena);

    int rc = 0;
    for (;;) {
        node_t *node = parser_parse(&par);
        if (!node) break;
        if (par.error) break;

        if (!sh->opt_n) {
            rc = exec_node(sh, node);
            sh->last_exit = rc;
            arena_reset(&sh->scratch_arena);
            if (errexit_should_stop(sh, rc))
                break;
        }
    }

    lexer_free(&lex);
    fclose(fp);
    return sh->last_exit;
}

/* -------------------------------------------------------------------------
 * shell_run_stdin
 * ------------------------------------------------------------------------- */

int shell_run_stdin(shell_ctx_t *sh)
{
    int interactive = isatty(STDIN_FILENO);

    lexer_t  lex;
    parser_t par;

    lexer_init_fp(&lex, stdin, &sh->parse_arena);
    parser_init(&par, &lex, &sh->parse_arena);

    int rc = 0;
    for (;;) {
        if (interactive) {
            const char *ps1 = vars_get(&sh->vars, "PS1");
            if (!ps1) ps1 = "$ ";
            fputs(ps1, stdout);
            fflush(stdout);
        }

        node_t *node = parser_parse(&par);
        if (!node) break;
        if (par.error) {
            par.error = 0;
            continue;
        }

        if (!sh->opt_n) {
            rc = exec_node(sh, node);
            sh->last_exit = rc;
            arena_reset(&sh->scratch_arena);
            if (errexit_should_stop(sh, rc))
                break;
        }

        /* Non-interactive: stop on EOF */
        if (!interactive) {
            /* parser_parse returns NULL on EOF; loop will break naturally */
        }
    }

    lexer_free(&lex);
    return sh->last_exit;
}

/* -------------------------------------------------------------------------
 * shell_free
 * ------------------------------------------------------------------------- */

/* Declared in exec.c — free all PATH cache entries */
void path_cache_clear(shell_ctx_t *sh);

void shell_free(shell_ctx_t *sh)
{
    path_cache_clear(sh);
    arena_free(&sh->parse_arena);
    arena_free(&sh->scratch_arena);
    /* job list nodes were malloc'd */
    job_t *j = sh->jobs.head;
    while (j) {
        job_t *next = j->next;
        free(j);
        j = next;
    }
    sh->jobs.head = NULL;

    if (g_trap_shell == sh)
        g_trap_shell = NULL;
}
