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

/* Internal flow-control sentinels returned through exec_node's `int` result.
 * They MUST stay above the valid exit-status range (0-255) so a command's real
 * status (e.g. `return 255`, or 128+signal) is never mistaken for flow control.
 * Shared by exec.c and shell.c (which routes eval/`.`/top-level flow). */
#define FLOW_BREAK    1000
#define FLOW_CONTINUE 1001
#define FLOW_RETURN   1002

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
    int         opt_v;       /* set -v / -o verbose: echo input as it is read */
    int         in_ps4;      /* expanding $PS4 for a trace line: suppresses
                              * tracing, so a `$(...)` inside PS4 does not
                              * re-enter the tracer for its own command and
                              * expand PS4 again, forever */
    int         opt_f;       /* set -f: no glob */
    int         opt_pipefail;
    int         opt_n;       /* set -n: no execute */
    int         opt_m;       /* set -m: monitor mode (job control) */
    int         opt_C;       /* set -C / -o noclobber: '>' won't overwrite existing regular file */
    int         opt_a;       /* set -a / -o allexport: auto-export assigned variables */
    int         in_cond;     /* set -e exempt: inside if/while/until condition, ! operand */
    int         and_or_exempt; /* set -e exempt: && left-side failure caused short-circuit */
    char       *script_name; /* $0 */
    char      **positional;  /* $1..$N, NULL-terminated */
    int         positional_n;
    /* Storage backing `positional`, so `set --` can release the previous list
     * instead of abandoning it.
     *
     * NULL means "not ours to free": the initial list (parse_arena) and a
     * function's arguments (the caller's expansion) both live elsewhere. Only a
     * `set --` executed in the CURRENT frame sets these, and only then may the
     * old list be freed -- an enclosing frame's saved list must outlive us.
     *
     * Separate from `positional` because `shift` advances positional and
     * decrements positional_n; base/base_n stay put, so they remain the pointer
     * to free and the number of strings to free with it.
     */
    char      **positional_base;
    int         positional_base_n;
    struct {
        char *action;
        int   set_in_this_shell; /* 1 if set in this shell level (not inherited) */
        int   inherited;         /* subshell: parent's action kept ONLY so a
                                  * `trap` listing can report it (POSIX); it is
                                  * never executed and any trap modification
                                  * wipes all inherited entries. */
    } traps[NSIG];
    int         in_trap;           /* >0 while running a trap action */
    int         trap_entry_status; /* $? when the current trap action started:
                                    * a naked `exit` inside the action uses it */
    int         errexit_fired;     /* set -e stopped execution (see shell.c) */
    char      **history;           /* interactive command history (malloc'd) */
    int         history_n;
    int         history_cap;
    int         opt_nolog;         /* set -o nolog: stop recording history */
    /* set -o vi / -o emacs: the line-editing mode. Tracked and reported (a
     * script that saves and restores options with `set +o` must get its choice
     * back, and `set -o vi` must not fail); silex's REPL has no editing modes
     * to switch between, so nothing else consults them. Mutually exclusive,
     * like every shell that has them. */
    int         opt_vi;
    int         opt_emacs;
    int         opt_nonlexicalctrl; /* set -o nonlexicalctrl: break/continue
                                      * in a function act on the CALLER's
                                      * loops (dynamic scope; smoosh) */
    int         current_lineno;    /* line of the currently executing simple
                                    * command ($LINENO); configure scripts
                                    * self-modify via sed when it's missing */
    int         opt_h;             /* set -h: hash utilities named in function
                                    * bodies when the function is defined */
    int         expansion_abort;   /* interactive: an expansion error aborts
                                    * only the current command with this
                                    * status (see expansion_abort()) */
    int         disposable;        /* this shell IS a throwaway child (command
                                    * substitution): its final command may
                                    * tail-exec (see exec_in_place) */
    int         exec_in_place;     /* this process is a disposable child and the
                                    * command about to run is the last thing it
                                    * does: an external command may execv() in
                                    * place instead of fork+wait, keeping the
                                    * process identity ($!, $PPID of the child)
                                    * correct (smoosh background.pid & friends) */
    /* Function definitions: name -> node_t* */
    void       *funcs[256];  /* var_entry_t* array for func lookup */
    /* Alias definitions: name -> value string */
    void       *aliases[256]; /* alias_entry_t* array for alias lookup */
    pid_t       last_bg_pid; /* $! */
    pid_t       shell_pgid;  /* the shell's own process group, for job control */
    int         tty_fd;      /* controlling terminal fd (-1 if none), for tcsetpgrp */
    int         job_control; /* 1 once job control is fully set up (interactive+tty) */
    pid_t       shell_pid;   /* $$: PID of the main shell, captured once at init.
                              * POSIX requires $$ to stay constant in subshells,
                              * so it must NOT be re-read with getpid() -- a fork
                              * for `( )`, a pipeline, or $(...) would otherwise
                              * change it, breaking `case $(...) in ($$)`. */
    int         call_depth;  /* function call nesting depth (recursion guard) */
    int         trace_level; /* SILEX_TRACE=1: +cmd, =2: +[builtin/fork/module] tag */
    int         break_level; /* for break N / continue N; 0 = normal */
    int         loop_depth;  /* current loop nesting depth; 0 = not in loop */
    int         interactive; /* 1 if shell is interactive (stdin is tty), 0 otherwise */
    int         in_command_builtin; /* 1 if executing via 'command' prefix (disables special builtin semantics) */
    int         run_string_parse_error; /* set by shell_run_string on a parse
                                         * error; eval (a special builtin)
                                         * checks it to abort a non-interactive
                                         * shell per POSIX (smoosh
                                         * parse.eval.error) */
    /* Set while expanding a here-document body. The body expands like a
     * double-quoted string (parameter/command/arithmetic expansion happens) but
     * with two differences: `"` (and `'`) are literal, not quote delimiters, and
     * `$@`/`$*` are joined with IFS's first byte -- a here-doc is text, never
     * field-split, so no \x01 boundary is emitted. Without this, `<<EOF` bodies
     * had their quotes stripped (and a lone `'` swallowed the rest, so `$HOME`
     * went unexpanded), and `$*`/`$@` leaked internal 0x01 bytes. */
    int         in_heredoc;
    /* Set while expanding an assignment's right-hand side (var=WORD). Like a
     * here-doc, this is a NON-split context, so unquoted `$*`/`$@` join with
     * IFS's first byte instead of emitting \x01 field boundaries -- `var=$*`
     * must yield the concatenation, not leak internal markers. Unlike in_heredoc
     * it does NOT change quote handling (assignment RHS processes quotes). */
    int         in_assign;
    /* Set while expanding the WORD of a ${var-WORD} / ${var=WORD} / ${var+WORD}
     * / ${var?WORD} parameter expansion. That word forms a single value that the
     * surrounding context then field-splits, so an UNQUOTED $* or $@ inside it
     * joins with IFS's first byte (`${var=$*}` yields one joined field, not the
     * markers) -- but a QUOTED "$@" still yields separate fields, so unlike
     * in_assign this does NOT affect the quoted-"$@" branch. */
    int         pp_join_unquoted;
    /* Set while expanding the WORD of a ${var-WORD} etc. that is itself inside
     * double quotes (`"${v-...}"`). POSIX 2.6.2: the whole WORD stays in the
     * double-quoted context, so an embedded literal `"` is REDUNDANT and simply
     * removed -- it neither terminates the word nor toggles to a splitting
     * context (`"${v-"a${nl}b"}"` keeps the newline). Without this, expand_into
     * treats that `"` as the close of a double-quoted section and truncates the
     * word. Only consulted on the in_dquote=1 `"` path, so it is a no-op when the
     * enclosing ${...} is unquoted (there embedded `"` open real quoted regions). */
    int         pp_word_dq;
    /* Set while expanding a word that contained a quoted "$@" with no positional
     * parameters. POSIX 2.5.2: "$@" with zero positionals generates ZERO fields,
     * even though it is double-quoted -- unlike "$*", which generates one empty
     * field. Without this, `exec cmd "$0" "$@"` (the autosetup/jimsh idiom, and
     * sqlite's ./configure) passes a phantom empty argument. */
    int         at_expanded_empty;
    /* Set while expanding a word in which "$@"/"$*" emitted a \x01 field
     * boundary. The field splitter uses \x01 as an internal marker, but a
     * literal 0x01 byte can also appear in real data (command-substitution
     * output, a variable value -- modernish deliberately tests ^A). Splitting on
     * every \x01 dropped such bytes: `A=$(printf '\001'); : "$A"` lost the byte,
     * failing modernish's FTL_ROASSIGN/FTL_CASECC init checks. The splitter now
     * runs only when this flag confirms the \x01 came from "$@", not from data. */
    int         at_field_boundary;
    /* Quote-aware field splitting. A word that WILL be IFS-split (an unquoted
     * expansion somewhere in it) may still contain quoted sub-regions whose
     * bytes must not be split -- e.g. `${x+"a b"}` is one field `a b`, and
     * `${x+A"$v"B}` with v="p q" is the single field `Ap qB`. A coarse
     * whole-word "does it split" flag cannot express that; dash tracks quoting
     * per character. expand_into() brackets each quoted region of such a word
     * with in-band QG_OPEN/QG_CLOSE bytes (\x02/\x03); the field splitter then
     * protects the bytes between them and strips the markers.
     *
     * emit_guards is set ONLY for words that will be split, so a fully-quoted
     * word (`"$v"`, `"$@"`) emits no markers and control-byte data in it is
     * never touched. quote_guard_depth coalesces nested quotes to a single
     * region (so a literal marker in data cannot unbalance the emitter), and
     * at_quote_guard tells the splitter markers are present to honor and strip. */
    int         emit_guards;       /* 1 while expanding a word that will IFS-split */
    int         quote_guard_depth; /* nesting depth of open quoted regions */
    int         at_quote_guard;    /* set once a QG_OPEN was actually emitted */
    /* Value of at_expanded_empty when the current outermost quoted region
     * opened. An empty region normally leaves a placeholder so it still counts
     * as a field, but a `"$@"` with no positional parameters must vanish
     * without one (POSIX: zero fields, not one empty field) -- comparing
     * against this tells the two apart even in a mixed word like `$x"$@"`. */
    int         qg_empty_at_open;
    /* Set while expanding the inside of a `${...}`. A literal IFS character
     * typed in the WORD of a `${v-word}` is part of the expansion's result and
     * so IS split (`IFS=x; echo ${v:-AxBxC}` prints three fields), whereas the
     * same character typed in the word around the substitution is not. Both
     * arrive at expand_into() as plain literal text; this is what tells them
     * apart. */
    int         in_subst_word;
    /* Set by the `sh -c STRING` entry point for the ONE shell_run_string call
     * that runs the script itself. There a stray `break`, `continue` or
     * `return` has no enclosing loop or function to act on: it must not be
     * propagated to the caller, where the sentinel became the shell's exit
     * status (`sh -c 'return'` exited 234, i.e. 1002 & 0xff). Consumed on
     * entry so that the eval arguments and trap actions nested inside -- which
     * DO propagate, as `f() { eval "return 3"; }` needs -- never see it. */
    int         cmd_string_top;
    /* Set when the shell stopped because the SCRIPT would not parse. A syntax
     * error is the shell's own failure and exits 2 whatever else happens; an
     * EXIT trap that runs on the way out must not overwrite that status with
     * its own, or `trap 'echo cleaning' EXIT` turns every syntax error into a
     * successful run. */
    int         script_parse_error;
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
    /* Raised alongside last_cmdsub_exit. A status of 0 cannot be told from
     * "no substitution ran at all" by value, and the two differ: with no
     * command name the assignments' substitution outranks the words', so
     * exec_simple_cmd has to know whether one happened, not just what it
     * returned. */
    int         last_cmdsub_seen;
    /* PATH resolution cache: command name → resolved absolute path.
     * Invalidated (path_cache_hash reset) when PATH changes. */
    void       *path_cache[256];  /* path_cache_entry_t*, open-addressing by FNV-1a */
    /* Applet names executed this session, for `hash` listing (applets never
     * enter the path cache -- they run in-process). Cleared by `hash -r`. */
    const char *applets_seen[64];
    int         applets_seen_n;
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
/* Record one command in the interactive history (no-op when not interactive
 * or under set -o nolog). Takes ownership of nothing; copies the text. */
void shell_history_add(shell_ctx_t *sh, const char *text);

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
