#ifndef MATCHBOX_SHELL_H
#define MATCHBOX_SHELL_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../util/arena.h"
#include "vars.h"
#include "job.h"
#include <signal.h>

/* NSIG is a Linux extension; provide a safe fallback for strict POSIX builds */
#ifndef NSIG
#define NSIG 64
#endif

#define SHELL_TRAP_DEFAULT NULL
#define SHELL_TRAP_IGNORE  ""

typedef struct shell_ctx {
    vars_t      vars;
    arena_t     parse_arena;
    job_list_t  jobs;
    int         last_exit;   /* $? */
    int         opt_e;       /* set -e */
    int         opt_u;       /* set -u */
    int         opt_x;       /* set -x */
    int         opt_f;       /* set -f: no glob */
    int         opt_pipefail;
    int         opt_n;       /* set -n: no execute */
    char       *script_name; /* $0 */
    char      **positional;  /* $1..$N, NULL-terminated */
    int         positional_n;
    struct {
        char *action;
    } traps[NSIG];
    /* Function definitions: name -> node_t* */
    void       *funcs[256];  /* var_entry_t* array for func lookup */
    pid_t       last_bg_pid; /* $! */
} shell_ctx_t;

int shell_init(shell_ctx_t *sh, int argc, char **argv);
int shell_run_string(shell_ctx_t *sh, const char *script);
int shell_run_file(shell_ctx_t *sh, const char *path);
int shell_run_stdin(shell_ctx_t *sh);
void shell_free(shell_ctx_t *sh);

/* Signal handler installed by trap built-in */
void shell_signal_handler(int sig);

#endif
