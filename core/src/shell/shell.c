/* shell.c — shell initialization, main loop, and cleanup */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "shell.h"
#include "exec.h"
#include "expand.h"
#include "../util/arena.h"
#include "../util/strbuf.h"

#include <ctype.h>
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
    if (g_trap_shell->traps[sig].inherited) return;  /* display-only */

    /* Run the trap action string. POSIX: $? is restored after a signal trap
     * action, and a naked `exit` inside it uses the pre-trap status. Under
     * set -e a failing command in the action exits the shell (smoosh
     * semantics.errexit.trap). */
    shell_ctx_t *sh = g_trap_shell;
    int save_status = sh->last_exit;
    sh->in_trap++;
    sh->trap_entry_status = save_status;
    sh->errexit_fired = 0;
    int rc = shell_run_string(sh, action);
    sh->in_trap--;
    if (sh->errexit_fired && !sh->interactive) {
        sh->errexit_fired = 0;
        exit(rc & 0xff ? rc & 0xff : 1);
    }
    sh->last_exit = save_status;
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
    int stop = sh->opt_e && rc != 0 && !sh->in_cond && !exempt;
    if (stop)
        sh->errexit_fired = 1;   /* lets a trap-action caller see the -e stop */
    return stop;
}

/* -------------------------------------------------------------------------
 * shell_init
 * ------------------------------------------------------------------------- */

int shell_init(shell_ctx_t *sh, int argc, char **argv)
{
    memset(sh, 0, sizeof(*sh));

    sh->shell_pid   = getpid();  /* $$: captured once; stable across subshells */
    sh->shell_pgid  = getpgrp();
    sh->tty_fd      = -1;
    sh->job_control = 0;
    arena_init(&sh->parse_arena, "parse");
    arena_init(&sh->scratch_arena, "scratch");
    sh->scratch = &sh->scratch_arena;
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

    /* $1..$N. These live in parse_arena, so the base stays NULL: the list is
     * not malloc'd and must never be passed to free(). The first `set --`
     * replaces it with an owned one. */
    sh->positional_base   = NULL;
    sh->positional_base_n = 0;
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
    if (pwd) {
        vars_set(&sh->vars, "PWD", pwd);
    } else {
        /* If PWD not set in environment, initialize to current directory */
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)))
            vars_set(&sh->vars, "PWD", cwd);
    }

    /* POSIX: PPID shall be set to the decimal value of the parent process ID */
    char ppid_buf[32];
    snprintf(ppid_buf, sizeof(ppid_buf), "%d", (int)getppid());
    vars_set(&sh->vars, "PPID", ppid_buf);

    /* POSIX shell variables with mandated default values. Setting these up front
     * matters under `set -u`: a script that references $OPTIND (before any
     * getopts) or $PS4 must see the default, not an "unbound variable" error.
     * Only set each if the environment didn't already provide it. */
    if (!vars_get(&sh->vars, "OPTIND")) vars_set(&sh->vars, "OPTIND", "1");
    if (!vars_get(&sh->vars, "PS1"))    vars_set(&sh->vars, "PS1", "$ ");
    if (!vars_get(&sh->vars, "PS2"))    vars_set(&sh->vars, "PS2", "> ");
    if (!vars_get(&sh->vars, "PS4"))    vars_set(&sh->vars, "PS4", "+ ");

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

    /* SIGPIPE stays at its default disposition: POSIX requires a shell (and a
     * `sh -c` script) to be killable by SIGPIPE and to report status 128+13, so
     * `case $(exec sh -c 'kill -s PIPE $$') in ...` works -- modernish probes
     * exactly this. It was previously SIG_IGN, so silex silently survived
     * SIGPIPE and reported 0. Pipeline stages fork, so a builtin on the left of
     * a broken pipe dies in its own child rather than taking down the shell;
     * SIG_DFL (not merely leaving it inherited) guarantees the right disposition
     * even when the parent that exec'd us had SIGPIPE ignored. */
    signal(SIGPIPE, SIG_DFL);

    /* Register as the global trap shell */
    g_trap_shell = sh;

    return 0;
}

void shell_history_add(shell_ctx_t *sh, const char *text)
{
    if (!sh->interactive || sh->opt_nolog || !text || !*text)
        return;
    if (sh->history_n >= sh->history_cap) {
        int ncap = sh->history_cap ? sh->history_cap * 2 : 64;
        char **nh = realloc(sh->history, (size_t)ncap * sizeof(char *));
        if (!nh) return;
        sh->history = nh;
        sh->history_cap = ncap;
    }
    sh->history[sh->history_n] = strdup(text);
    if (sh->history[sh->history_n]) sh->history_n++;
}

/* -------------------------------------------------------------------------
 * shell_run_string
 * ------------------------------------------------------------------------- */

int shell_run_string(shell_ctx_t *sh, const char *script)
{
    if (!script) return 0;

    lexer_t  lex;
    parser_t par;

    /* Two private arenas, both freed on the way out instead of piling into the
     * shell's persistent parse_arena:
     *
     *   parse_local  backs the lexer/parser. `eval` re-runs this per command,
     *                so `LOOP ...; DO eval "..."; DONE` (modernish) used to parse
     *                a fresh tree into parse_arena every iteration and never
     *                reclaim it -- a long loop walked parse_arena up to its 64 MB
     *                cap and aborted. Freeing parse_local here bounds an eval to
     *                O(one call). The one thing that must outlive it is a function
     *                BODY defined by the eval; func_register() deep-copies that
     *                into parse_arena (node_dup), so freeing parse_local is safe.
     *                It is NOT reset mid-loop: the parser keeps live state
     *                (lookahead, pending heredocs) in it between parser_parse()
     *                calls.
     *
     *   local        is sh->scratch: expansions for the command currently
     *                executing. Reset per parse unit, as before.
     *
     * Both are private (vs reclaiming the caller's sh->scratch) because this is
     * re-entrant: `eval`, traps and `.` route back here while an outer command
     * still executes, and stomping the caller's scratch would free e.g. an
     * enclosing for loop's word list mid-iteration. `script` is expanded from the
     * caller's arena, which this leaves untouched, so it stays valid for the lexer.
     */
    arena_t  parse_local;
    arena_t  local;
    arena_t *saved_scratch = sh->scratch;
    arena_init(&parse_local, "run-string-parse");
    arena_init(&local, "run-string");
    /* Not a dangling pointer: saved_scratch is restored on every path out of
     * this function, including the parse-error and EOF breaks, so sh->scratch
     * never outlives `local`. cppcheck flags the assignment itself because it
     * cannot see the restore below. */
    /* cppcheck-suppress autoVariables */
    sh->scratch = &local;

    /* Code run here -- an eval argument, a trap action, a command substitution --
     * is its own command context and must see normal function lookup, even when
     * this call is reached while a `command` prefix is active. Without clearing
     * the flag, `command exit` firing the EXIT trap ran the trap's commands with
     * functions bypassed (`_Msh_doTraps: command not found`), and a signal trap
     * arriving during `command foo` did the same. Restored on exit so an
     * interrupted `command foo` resumes correctly. */
    int saved_icb = sh->in_command_builtin;
    sh->in_command_builtin = 0;

    lexer_init_str(&lex, script, &parse_local);
    parser_init(&par, &lex, &parse_local);
    parser_set_aliases(&par, shell_alias_lookup_cb, sh);

    int rc = 0;
    sh->run_string_parse_error = 0;
    for (;;) {
        node_t *node = parser_parse(&par);
        if (par.error) {
            sh->last_exit = 2;  /* Parse error returns exit code 2 */
            sh->run_string_parse_error = 1;
            break;
        }
        if (!node) break;  /* EOF */

        if (!sh->opt_n) {
            /* Disposable child (command substitution): its LAST command may
             * tail-exec so the executed program keeps this process identity
             * ($PPID reads the real parent -- smoosh semantics.backtick.ppid).
             * Not if any trap is armed: exec would silently drop it. */
            if (sh->interactive) {
                char *h_ = describe_node(node);
                shell_history_add(sh, h_);
                free(h_);
            }
            if (sh->disposable && node->type == N_CMD && parser_at_eof(&par)) {
                int has_trap = 0;
                for (int ti = 0; ti < NSIG; ti++)
                    if (sh->traps[ti].set_in_this_shell &&
                        sh->traps[ti].action != SHELL_TRAP_DEFAULT) {
                        has_trap = 1;
                        break;
                    }
                if (!has_trap)
                    sh->exec_in_place = 1;
            }
            rc = exec_node(sh, node);
            sh->exec_in_place = 0;
            /* Flow control (FLOW_BREAK / FLOW_CONTINUE / FLOW_RETURN) must
             * propagate to the CALLER, because this runs the argument of `eval`,
             * which is transparent to return/break/continue -- they act on the
             * function or loop that encloses the eval. Storing the sentinel in
             * last_exit and continuing (as before) meant `f() { eval "return 3"; }`
             * never returned from f; the sentinel leaked upward and exited the
             * shell. modernish's FTL_EVALRET / FTL_EVALCOBR checks exercise this. */
            if (rc >= FLOW_BREAK && rc <= FLOW_RETURN) {
                sh->scratch = saved_scratch;
                sh->in_command_builtin = saved_icb;
                arena_free(&local);
                arena_free(&parse_local);
                lexer_free(&lex);
                return rc;
            }
            sh->last_exit = rc;
            /* Reclaim scratch after each parse unit. Note parser_parse() returns
             * a whole `a; b; c`-style list as ONE node, so for a typical script
             * this loop runs about once and the reset fires about once per input,
             * not once per command. Intra-input accumulation is bounded instead
             * by the per-iteration arena a loop body gets (see loop_scratch_* in
             * exec.c) and the private arena eval/`.` run in, so a long-running
             * loop or sourced script does not pile expansions up here. */
            arena_reset(&local);
            if (errexit_should_stop(sh, rc))
                break;
        }
    }

    sh->scratch = saved_scratch;
    sh->in_command_builtin = saved_icb;
    arena_free(&local);
    arena_free(&parse_local);

    lexer_free(&lex);
    return sh->last_exit;
}

/* -------------------------------------------------------------------------
 * shell_run_file
 * ------------------------------------------------------------------------- */

int shell_run_file(shell_ctx_t *sh, const char *path)
{
    /* Running from a file is normally non-interactive, but an explicit -i
     * keeps interactive semantics (sh.interactive preset by the caller;
     * smoosh builtin.readonly.assign.interactive runs `sh -i scr` and expects
     * errors to abort only the offending command). Don't clobber it here --
     * `.` also routes through this function and must keep the CALLER's mode. */
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        sh->last_exit = 1;
        return 1;
    }

    /* Slurp the whole script into memory and lex from that string, rather than
     * streaming the FILE* with the lexer.
     *
     * WHY: the parser now runs one command at a time (parser_parse returns a
     * single command so an alias/`use`/function it defines is visible to the
     * NEXT command — POSIX interleaving). That means we EXECUTE commands while
     * the script is still being read. A command that forks — a subshell, a
     * command substitution, or the module loads modernish does at the top of a
     * file (`use var/shellquote` in readlink.mm) — hands the child a copy of
     * this FILE*. With buffered stdio the parent has read ahead, so the child
     * shares an fd whose offset is past the parse point; when the child exits,
     * glibc's stdio cleanup lseek()s that shared fd BACK by the amount still
     * sitting unconsumed in the buffer. The parent then re-reads bytes it
     * already buffered, so the tokenizer sees the file's earlier content spliced
     * into a later line (a single-quoted glob pattern swallowing the file
     * header, etc.). Batch parsing never hit this because it read the entire
     * file before any command — hence any fork — ran.
     *
     * Reading it all up front removes the shared, mid-file fd entirely: the fp
     * is closed before we execute anything, and token text is copied into the
     * arena, so the buffer only needs to outlive parsing. (setvbuf(_IONBF) also
     * fixes the corruption but costs a syscall per byte across every sourced
     * module; slurping is both correct and fast.) */
    size_t  cap = 0, len = 0;
    char   *src = NULL;
    for (;;) {
        if (len + 65536 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 65536 + 1;
            char  *nb   = realloc(src, ncap);
            if (!nb) { free(src); fclose(fp); sh->last_exit = 1; return 1; }
            src = nb; cap = ncap;
        }
        size_t got = fread(src + len, 1, 65536, fp);
        len += got;
        if (got < 65536) {
            if (ferror(fp)) { free(src); fclose(fp); perror(path); sh->last_exit = 1; return 1; }
            break;  /* EOF */
        }
    }
    fclose(fp);
    fp = NULL;
    if (!src) {                     /* empty file */
        src = malloc(1);
        if (!src) { sh->last_exit = 1; return 1; }
        len = 0;
    }
    src[len] = '\0';

    lexer_t  lex;
    parser_t par;

    /* parse_local backs the parser and is freed when the file finishes, so a
     * sourced module's parse tree does not stay pinned in the persistent
     * parse_arena for the life of the shell. Any function it defines is
     * deep-copied into parse_arena by func_register(), so freeing parse_local is
     * safe. It is not reset mid-loop (the parser keeps live state in it). `local`
     * is sh->scratch, reset per parse unit. Both private for the same reentrancy
     * reason as shell_run_string: `.` runs while the caller is mid-command. */
    arena_t  parse_local;
    arena_t  local;
    arena_t *saved_scratch = sh->scratch;
    arena_init(&parse_local, "run-file-parse");
    arena_init(&local, "run-file");

    lexer_init_str(&lex, src, &parse_local);
    parser_init(&par, &lex, &parse_local);
    parser_set_aliases(&par, shell_alias_lookup_cb, sh);

    /* Restored on every path out, including the FLOW_BREAK/FLOW_CONTINUE early
     * return below. See shell_run_string. */
    /* cppcheck-suppress autoVariables */
    sh->scratch = &local;

    int rc = 0;
    for (;;) {
        node_t *node = parser_parse(&par);
        if (par.error) {
            sh->last_exit = 2;  /* Parse error returns exit code 2 */
            break;
        }
        if (!node) break;  /* EOF */

        if (!sh->opt_n) {
            if (sh->interactive) {
                char *h_ = describe_node(node);
                shell_history_add(sh, h_);
                free(h_);
            }
            rc = exec_node(sh, node);
            /* FLOW_RETURN inside a sourced script acts like exit from the script */
            if (rc == FLOW_RETURN) {
                /* return builtin already set sh->last_exit; just break out */
                break;
            }
            /* FLOW_BREAK / FLOW_CONTINUE must propagate to the caller's loop */
            if (rc == FLOW_BREAK || rc == FLOW_CONTINUE) {
                /* Don't update sh->last_exit; propagate flow control code */
                sh->scratch = saved_scratch;
                arena_free(&local);
                arena_free(&parse_local);
                lexer_free(&lex);
                free(src);
                return rc;
            }
            sh->last_exit = rc;
            arena_reset(&local);
            if (errexit_should_stop(sh, rc))
                break;
        }
    }

    sh->scratch = saved_scratch;
    arena_free(&local);
    arena_free(&parse_local);

    lexer_free(&lex);
    free(src);
    return sh->last_exit;
}

/* -------------------------------------------------------------------------
 * shell_run_stdin
 * ------------------------------------------------------------------------- */

/* Set up job control for an interactive shell with a controlling terminal:
 * take our own process group, own the terminal, and ignore the job-control
 * signals so a foreground child's SIGTSTP stops the child, not us. Silently
 * does nothing (job_control stays 0) if there is no tty or setup is not allowed
 * -- a non-interactive or piped shell then behaves exactly as before. */
/* Set by ^C while the interactive shell waits at its prompt, so the input loop
 * can discard the half-typed line and redraw a fresh prompt instead of dying. */
static volatile sig_atomic_t interactive_sigint = 0;
static void interactive_sigint_handler(int sig) { (void)sig; interactive_sigint = 1; }

static void shell_init_job_control(shell_ctx_t *sh)
{
    if (!sh->interactive || !isatty(STDIN_FILENO))
        return;
    int tty = STDIN_FILENO;

    /* If launched in the background, stop until we are in the foreground. */
    pid_t pgrp;
    while ((pgrp = tcgetpgrp(tty)) != -1 && pgrp != getpgrp()) {
        if (kill(-getpgrp(), SIGTTIN) != 0)
            break;
    }

    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    /* At the prompt, ^C must cancel the current input line, not kill the shell.
     * sigaction without SA_RESTART so the getline() read returns EINTR; a
     * foreground child still gets a default SIGINT (it resets the disposition
     * after fork and owns the terminal while it runs). */
    struct sigaction sa;
    sa.sa_handler = interactive_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    sh->shell_pgid = getpid();
    if (setpgid(sh->shell_pgid, sh->shell_pgid) < 0 && errno != EPERM)
        return;                       /* cannot form our own group; skip */
    if (tcsetpgrp(tty, sh->shell_pgid) < 0)
        return;                       /* cannot own the terminal; skip */

    sh->tty_fd      = tty;
    sh->job_control = 1;
    sh->opt_m       = 1;              /* monitor mode on by default when interactive */
}

/* Heuristic: does `s` end in the middle of a construct, so the interactive REPL
 * should read another line (PS2) before parsing? Covers the common cases --
 * open quotes, a trailing line-continuation backslash, unbalanced (), `` ` ``,
 * ${...}/$(...), and unterminated compound commands (if/for/while/until/case/{).
 * Being wrong is benign: a false "complete" just lets the parser report the
 * error, and a false "incomplete" waits for a line the user can abort with ^C. */
static int input_incomplete(const char *s)
{
    int sq = 0, dq = 0, backtick = 0, paren = 0, brace = 0, compound = 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (sq) { if (c == '\'') sq = 0; continue; }
        if (dq) {
            if (c == '\\' && i + 1 < n) i++;
            else if (c == '"') dq = 0;
            continue;
        }
        if (c == '\\') { if (i + 1 >= n) return 1; i++; continue; }
        if (c == '\'') { sq = 1; continue; }
        if (c == '"')  { dq = 1; continue; }
        if (c == '`')  { backtick ^= 1; continue; }
        if (c == '#') {                 /* comment: skip to end of line */
            while (i + 1 < n && s[i + 1] != '\n') i++;
            continue;
        }
        if (c == '(') { paren++; continue; }
        if (c == ')') { if (paren > 0) paren--; continue; }
        /* Reserved words at a word boundary. */
        if ((isalpha((unsigned char)c) || c == '{' || c == '}') &&
            (i == 0 || isspace((unsigned char)s[i-1]) ||
             s[i-1] == ';' || s[i-1] == '&' || s[i-1] == '|')) {
            size_t j = i;
            if (c == '{' || c == '}') j = i + 1;
            else while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            size_t len = j - i;
            /* the word must end at a boundary too */
            int boundary = (j >= n || isspace((unsigned char)s[j]) ||
                            s[j] == ';' || s[j] == '&' || s[j] == '|');
            if (boundary) {
                #define KW(w) (len == strlen(w) && strncmp(s + i, w, len) == 0)
                if (KW("if") || KW("for") || KW("while") || KW("until") || KW("case"))
                    compound++;
                else if (KW("fi") || KW("done") || KW("esac"))
                    compound--;
                else if (KW("{")) brace++;
                else if (KW("}")) brace--;
                #undef KW
            }
            if (len > 0) i = j - 1;
        }
    }
    return sq || dq || backtick || paren > 0 || brace > 0 || compound > 0;
}

/* Interactive REPL: read a whole command (with PS2 continuation) into a buffer
 * and parse it from a string, so a command runs on its own Enter. Reading via
 * the fp lexer instead made parse_list peek past the newline for the next
 * command, blocking on the next line -- so each command ran one line late. */
static int shell_run_interactive(shell_ctx_t *sh)
{
    char   *buf = NULL;
    size_t  bufcap = 0, buflen = 0;
    char   *line = NULL;
    size_t  linecap = 0;
    int     rc = 0;

    for (;;) {
        const char *prompt;
        if (buflen == 0) {
            prompt = vars_get(&sh->vars, "PS1");
            if (!prompt) prompt = "$ ";
        } else {
            prompt = vars_get(&sh->vars, "PS2");
            if (!prompt) prompt = "> ";
        }
        /* POSIX: prompts go to STANDARD ERROR (smoosh expects `$ ` on the
         * .err channel), and stdout must be flushed first so output and
         * prompts interleave correctly. */
        fflush(stdout);
        fputs(prompt, stderr);
        fflush(stderr);

        interactive_sigint = 0;
        ssize_t r = getline(&line, &linecap, stdin);
        if (r < 0) {
            if (errno == EINTR && interactive_sigint) {
                /* ^C at the prompt: drop the pending line, redraw PS1. */
                interactive_sigint = 0;
                clearerr(stdin);
                buflen = 0;
                putchar('\n');
                continue;
            }
            if (buflen > 0) putchar('\n');
            break;                      /* EOF (^D) */
        }

        /* Append the line to the pending command buffer. */
        if (buflen + (size_t)r + 1 > bufcap) {
            size_t ncap = (buflen + (size_t)r + 1) * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); free(line); return 1; }
            buf = nb; bufcap = ncap;
        }
        memcpy(buf + buflen, line, (size_t)r);
        buflen += (size_t)r;
        buf[buflen] = '\0';

        if (input_incomplete(buf))
            continue;                   /* read another line under PS2 */

        /* Record the raw line in the history, then parse and run it. */
        {
            size_t hl = strlen(buf);
            while (hl && buf[hl-1] == '\n') buf[--hl] = '\0';
            shell_history_add(sh, buf);
            if (hl < bufcap) buf[hl] = '\0';
        }
        lexer_t  lex;
        parser_t par;
        lexer_init_str(&lex, buf, &sh->parse_arena);
        parser_init(&par, &lex, &sh->parse_arena);
        parser_set_aliases(&par, shell_alias_lookup_cb, sh);
        for (;;) {
            node_t *node = parser_parse(&par);
            if (par.error) { sh->last_exit = 2; par.error = 0; break; }
            if (!node) break;
            if (!sh->opt_n) {
                rc = exec_node(sh, node);
                sh->last_exit = rc;
                arena_reset(&sh->scratch_arena);
            }
        }
        lexer_free(&lex);
        buflen = 0;
    }

    free(buf);
    free(line);
    return sh->last_exit;
}

int shell_run_stdin(shell_ctx_t *sh)
{
    /* -i forces interactive even when stdin is a pipe/heredoc (POSIX; smoosh
     * sh.ps1.override): prompts are printed and errors don't abort the shell.
     * Otherwise auto-detect from the terminal. */
    int interactive = sh->interactive || isatty(STDIN_FILENO);
    sh->interactive = interactive;
    shell_init_job_control(sh);

    if (interactive)
        return shell_run_interactive(sh);

    /* Unbuffered so the fd offset always matches exactly what the parser has
     * consumed. A non-interactive stdin script is read command-by-command and
     * executed as we go; a forked child (subshell/command substitution) shares
     * this fd, and buffered read-ahead would let the child's exit-time stdio
     * cleanup lseek it back over the unconsumed buffer, corrupting the parse.
     * See the detailed note in shell_run_file. stdin can't be slurped up front
     * (it may stream), so keep the fd position honest instead. It also means a
     * `read` in the script consumes exactly its bytes and no more. */
    setvbuf(stdin, NULL, _IONBF, 0);

    lexer_t  lex;
    parser_t par;

    lexer_init_fp(&lex, stdin, &sh->parse_arena);
    parser_init(&par, &lex, &sh->parse_arena);
    parser_set_aliases(&par, shell_alias_lookup_cb, sh);

    int rc = 0;
    for (;;) {
        node_t *node = parser_parse(&par);
        /* Check the error flag BEFORE the NULL check: a parse error returns
         * NULL, and testing !node first silently swallowed the error, so
         * `echo 'eval )' | silex` exited 0 instead of 2 (smoosh parse.error). */
        if (par.error) {
            sh->last_exit = 2;  /* Parse error in non-interactive mode */
            break;
        }
        if (!node) break;

        if (!sh->opt_n) {
            rc = exec_node(sh, node);
            sh->last_exit = rc;
            arena_reset(&sh->scratch_arena);
            if (errexit_should_stop(sh, rc))
                break;
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
/* Declared in exec.c — free the positional list if this shell owns it */
void positional_free(shell_ctx_t *sh);

void shell_free(shell_ctx_t *sh)
{
    for (int hi = 0; hi < sh->history_n; hi++)
        free(sh->history[hi]);
    free(sh->history);
    sh->history = NULL;

    path_cache_clear(sh);
    positional_free(sh);
    arena_free(&sh->parse_arena);
    arena_free(&sh->scratch_arena);
    job_list_free(&sh->jobs);   /* frees job nodes and their command strings */

    if (g_trap_shell == sh)
        g_trap_shell = NULL;
}

/* -------------------------------------------------------------------------
 * sh_parse_int: strict integer parse for user-supplied numbers.
 * See shell.h for why atoi() is not good enough.
 * ------------------------------------------------------------------------- */
int sh_parse_int(const char *s, int min, int max, int *out)
{
    if (!s || !*s)
        return -1;

    /* Leading whitespace is accepted (strtol does), trailing garbage is not. */
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);

    if (end == s)          return -1;   /* no digits at all */
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0')      return -1;   /* trailing garbage: "12abc" */
    if (errno == ERANGE)   return -1;   /* out of long range */
    if (v < (long)min || v > (long)max) return -1;

    *out = (int)v;
    return 0;
}
