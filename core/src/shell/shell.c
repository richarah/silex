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

    /* Run in a private arena rather than reclaiming the caller's.
     *
     * This is re-entrant: `eval`, traps and `.` all route back through here
     * while an outer command is still executing. Resetting sh->scratch_arena
     * here therefore freed memory the *caller* still owned, and
     *
     *     for x in alpha bravo charlie; do eval "true"; echo "$x"; done
     *
     * printed "alpha", then "pha", then stopped -- eval reset the arena holding
     * the for loop's word list, and the loop walked freed memory. It corrupted
     * silently instead of crashing only because a reset arena keeps its blocks
     * mapped, so the stale read usually finds the old bytes still there.
     *
     * `script` itself is expanded from the caller's arena, which this leaves
     * untouched, so it stays valid for the lexer. Nothing a command produces
     * outlives the command in this arena: variables and positionals are copied
     * into parse_arena.
     */
    arena_t  local;
    arena_t *saved_scratch = sh->scratch;
    arena_init(&local, "run-string");
    /* Not a dangling pointer: saved_scratch is restored on every path out of
     * this function, including the parse-error and EOF breaks, so sh->scratch
     * never outlives `local`. cppcheck flags the assignment itself because it
     * cannot see the restore below. */
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
            rc = exec_node(sh, node);
            /* Flow control (FLOW_BREAK 200 / FLOW_CONTINUE 201 / FLOW_RETURN
             * 202) must propagate to the CALLER, because this runs the argument
             * of `eval`, which is transparent to return/break/continue -- they
             * act on the function or loop that encloses the eval. Storing the
             * 202 sentinel in last_exit and continuing (as before) meant
             * `f() { eval "return 3"; }` never returned from f; the sentinel
             * leaked upward and exited the shell. modernish's FTL_EVALRET /
             * FTL_EVALCOBR checks exercise exactly this. */
            if (rc >= 200 && rc <= 202) {
                sh->scratch = saved_scratch;
                arena_free(&local);
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
    arena_free(&local);

    lexer_free(&lex);
    return sh->last_exit;
}

/* -------------------------------------------------------------------------
 * shell_run_file
 * ------------------------------------------------------------------------- */

int shell_run_file(shell_ctx_t *sh, const char *path)
{
    sh->interactive = 0;  /* Running from file: non-interactive */
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

    /* Private arena, for the same reason as shell_run_string: `.` re-enters this
     * while the caller is mid-command, so reclaiming sh->scratch_arena here
     * would free memory the caller still owns (e.g. an enclosing for loop's
     * word list). */
    arena_t  local;
    arena_t *saved_scratch = sh->scratch;
    arena_init(&local, "run-file");
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
            rc = exec_node(sh, node);
            /* FLOW_RETURN (202) inside sourced script acts like exit from the script */
            if (rc == 202) {  /* FLOW_RETURN */
                /* return builtin already set sh->last_exit; just break out */
                break;
            }
            /* FLOW_BREAK (200) and FLOW_CONTINUE (201) must propagate to caller's loop */
            if (rc == 200 || rc == 201) {  /* FLOW_BREAK or FLOW_CONTINUE */
                /* Don't update sh->last_exit; propagate flow control code */
                sh->scratch = saved_scratch;
                arena_free(&local);
                lexer_free(&lex);
                fclose(fp);
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

    lexer_free(&lex);
    fclose(fp);
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
        fputs(prompt, stdout);
        fflush(stdout);

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

        /* Parse and run the accumulated command from a string. */
        lexer_t  lex;
        parser_t par;
        lexer_init_str(&lex, buf, &sh->parse_arena);
        parser_init(&par, &lex, &sh->parse_arena);
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
    int interactive = isatty(STDIN_FILENO);
    sh->interactive = interactive;
    shell_init_job_control(sh);

    if (interactive)
        return shell_run_interactive(sh);

    lexer_t  lex;
    parser_t par;

    lexer_init_fp(&lex, stdin, &sh->parse_arena);
    parser_init(&par, &lex, &sh->parse_arena);

    int rc = 0;
    for (;;) {
        node_t *node = parser_parse(&par);
        if (!node) break;
        if (par.error) {
            sh->last_exit = 2;  /* Parse error in non-interactive mode */
            break;
        }

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
