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
    arena_t     scratch_arena; /* root scratch: expansion temporaries */
    /* Where expansions allocate right now. Normally &scratch_arena, but a loop
     * points it at a per-loop child arena so each iteration's expansions can be
     * reclaimed without touching anything the loop itself owns (e.g. a `for`
     * word list, or the positionals of an enclosing function call, both of
     * which are allocated from the parent scratch before the swap).
     *
     * Always allocate expansions through this pointer, never &scratch_arena. */
    arena_t    *scratch;
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
        int   set_in_this_shell; /* 1 if set in this shell level (not inherited) */
    } traps[NSIG];
    /* Function definitions: name -> node_t* */
    void       *funcs[256];  /* var_entry_t* array for func lookup */
    /* Alias definitions: name -> value string */
    void       *aliases[256]; /* alias_entry_t* array for alias lookup */
    pid_t       last_bg_pid; /* $! */
    int         call_depth;  /* function call nesting depth (recursion guard) */
    int         trace_level; /* SILEX_TRACE=1: +cmd, =2: +[builtin/fork/module] tag */
    int         break_level; /* for break N / continue N; 0 = normal */
    int         loop_depth;  /* current loop nesting depth; 0 = not in loop */
    int         interactive; /* 1 if shell is interactive (stdin is tty), 0 otherwise */
    int         in_command_builtin; /* 1 if executing via 'command' prefix (disables special builtin semantics) */
    /* Set while expanding a word that contained a quoted "$@" with no positional
     * parameters. POSIX 2.5.2: "$@" with zero positionals generates ZERO fields,
     * even though it is double-quoted -- unlike "$*", which generates one empty
     * field. Without this, `exec cmd "$0" "$@"` (the autosetup/jimsh idiom, and
     * sqlite's ./configure) passes a phantom empty argument. */
    int         at_expanded_empty;
    /* Exit status of the most recent command substitution performed during word
     * expansion. POSIX 2.9.1: a command with no command name but containing a
     * command substitution completes with the status of the LAST command
     * substitution performed -- so `v=$(false); echo $?` must print 1.
     *
     * Kept separate from last_exit deliberately: $? must NOT be disturbed by a
     * command substitution in an ordinary word (`echo $(false); echo $?` is
     * echo's status, i.e. 0). Only the assignment-only path in exec_simple_cmd
     * consumes this. */
    int         last_cmdsub_exit;
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

/* Strict integer parse for user-supplied numbers (exit codes, signal numbers,
 * file descriptors, shift/break/continue counts).
 *
 * atoi() returns 0 for anything it cannot parse and has no way to report an
 * error, so `exit abc` exited 0 -- a build step that failed reporting success.
 * It also cannot detect overflow.
 *
 * Returns 0 and stores the value on success; returns -1 on a trailing garbage,
 * empty string, or out-of-range input. Rejects values outside [min, max].
 */
int sh_parse_int(const char *s, int min, int max, int *out);

#endif
