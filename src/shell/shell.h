#ifndef SILEX_SHELL_H
#define SILEX_SHELL_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../util/arena.h"
#include "vars.h"
#include "job.h"
#include <signal.h>
#include <stdint.h>

/* NSIG is a Linux extension; provide a safe fallback for strict POSIX builds */
#ifndef NSIG
#define NSIG 64
#endif

#define SHELL_TRAP_DEFAULT NULL
#define SHELL_TRAP_IGNORE  ""

typedef struct shell_ctx {
    vars_t      vars;
    arena_t     parse_arena;   /* persistent: AST, tokens, func defs, traps */
    arena_t     scratch_arena; /* scratch: expansion temporaries; reset between top-level commands */
    job_list_t  jobs;
    int         last_exit;   /* $? */
    int         opt_e;       /* set -e */
    int         opt_u;       /* set -u */
    int         opt_x;       /* set -x */
    int         opt_f;       /* set -f: no glob */
    int         opt_pipefail;
    int         opt_n;       /* set -n: no execute */
    int         in_cond;     /* set -e exempt: inside if/while/until condition, ! operand */
    int         and_or_exempt; /* set -e exempt: && left-side failure caused short-circuit */
    char       *script_name; /* $0 */
    char      **positional;  /* $1..$N, NULL-terminated */
    int         positional_n;
    struct {
        char *action;
    } traps[NSIG];
    /* Function definitions: name -> node_t* */
    void       *funcs[256];  /* var_entry_t* array for func lookup */
    pid_t       last_bg_pid; /* $! */
    int         call_depth;  /* function call nesting depth (recursion guard) */
    int         trace_level; /* MATCHBOX_TRACE=1: +cmd, =2: +[builtin/fork/module] tag */
    int         break_level; /* for break N / continue N; 0 = normal */
    /* PATH resolution cache: command name → resolved absolute path.
     * Invalidated (path_cache_hash reset) when PATH changes. */
    void       *path_cache[256];  /* path_cache_entry_t*, open-addressing by FNV-1a */
    uint32_t    path_cache_hash;  /* FNV-1a hash of PATH string when cache was built */
} shell_ctx_t;

#define SHELL_MAX_CALL_DEPTH 1000

int shell_init(shell_ctx_t *sh, int argc, char **argv);
int shell_run_string(shell_ctx_t *sh, const char *script);
int shell_run_file(shell_ctx_t *sh, const char *path);
int shell_run_stdin(shell_ctx_t *sh);
void shell_free(shell_ctx_t *sh);

/* Signal handler installed by trap built-in */
void shell_signal_handler(int sig);

#endif
