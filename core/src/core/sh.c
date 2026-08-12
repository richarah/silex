/* sh.c — sh applet: entry point for the silex POSIX shell */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../shell/shell.h"
#include "../util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * applet_sh -- shell entry point
 *
 * Modes:
 *   sh                    interactive (stdin)
 *   sh SCRIPT [ARG...]    run script file
 *   sh -c CMD [ARG...]    run command string
 *
 * Options:
 *   -c CMD    command string
 *   -e        exit on error
 *   -u        error on undefined variable
 *   -x        trace
 *   -n        no execute (syntax check only)
 *   -f        disable globbing
 *   -i        interactive: forced on (otherwise auto-detected by isatty)
 *   -o OPT    set option by name
 *   --        end options
 */
int applet_sh(int argc, char **argv)
{
    const char *cmd_string = NULL;
    int         arg_start  = argc; /* default: no script file */
    int         opt_e = 0, opt_u = 0, opt_x = 0, opt_n = 0;
    int         opt_f = 0, opt_pipefail = 0, opt_i = 0;

    /* POSIX option parsing: options are read until `--`, `-`, or the first
     * OPERAND; -c does not itself consume an argument, the first operand
     * becomes the command string. That ordering matters:
     *   sh -c -x 'echo hi'   -> -x is a flag, 'echo hi' is the string
     *   sh -c -z             -> an illegal OPTION, not a command named -z
     *   sh -c -- 'echo hi'   -> `--` ends options, then the string
     *   sh -c 'echo' -z      -> -z is past the string: it is $0, not a flag
     * The old loop took argv[i+1] as the string the moment it saw -c, so all
     * four ran the wrong thing (`silex: --: command not found`). */
    int want_cmd_string = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            arg_start = i + 1;
            break;
        }
        /* A lone "-" ends option processing too (and is not an operand). */
        if (strcmp(argv[i], "-") == 0) {
            arg_start = i + 1;
            break;
        }
        /* `+x` turns an option off; `+c` is accepted as a synonym for -c
         * (dash runs `sh +c 'echo hi'`). */
        if (argv[i][0] != '-' && argv[i][0] != '+') {
            arg_start = i;
            break;
        }
        int set_on = (argv[i][0] == '-');
        const char *p = argv[i] + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'c':
                if (*(p + 1)) {
                    /* attached form: -cCMD */
                    cmd_string = p + 1;
                    p = p + strlen(p) - 1;
                    arg_start = i + 1;
                    stop = 1;
                } else {
                    want_cmd_string = 1;
                }
                break;
            case 'v': /* verbose: accepted (echoing input is not implemented) */
                break;
            case 'e': opt_e        = set_on; break;
            case 'u': opt_u        = set_on; break;
            case 'x': opt_x        = set_on; break;
            case 'n': opt_n        = set_on; break;
            case 'f': opt_f        = set_on; break;
            case 'i': opt_i        = set_on; break;
            case 'o': {
                const char *name = NULL;
                if (*(p + 1)) {
                    name = p + 1;
                    p = p + strlen(p) - 1;
                } else if (i + 1 < argc) {
                    name = argv[++i];
                    stop = 1;
                }
                if (name) {
                    if      (strcmp(name, "errexit")  == 0) opt_e = set_on;
                    else if (strcmp(name, "nounset")  == 0) opt_u = set_on;
                    else if (strcmp(name, "xtrace")   == 0) opt_x = set_on;
                    else if (strcmp(name, "pipefail") == 0) opt_pipefail = set_on;
                    else if (strcmp(name, "noglob")   == 0) opt_f = set_on;
                }
                break;
            }
            default:
                /* dash: "Illegal option -z", exit 2 */
                fprintf(stderr, "silex: %c%c: Illegal option %c%c\n",
                        set_on ? '-' : '+', *p, set_on ? '-' : '+', *p);
                return 2;
            }
            if (!stop) p++;
        }
    }

    /* -c was given without an attached string: the first operand is it, and
     * everything after that operand is $0, $1, ... */
    if (want_cmd_string && !cmd_string) {
        if (arg_start >= argc) {
            fprintf(stderr, "silex: sh: -c requires an argument\n");
            return 2;
        }
        cmd_string = argv[arg_start];
        arg_start++;
    }

    /* Build positional parameter argv for shell_init.
     * shell_init gets: argv[0] = $0, argv[1..] = $1.. */
    int   sh_argc;
    char **sh_argv;

    if (cmd_string) {
        /* POSIX `sh -c command_string [command_name [argument...]]`: the FIRST
         * operand after the command string becomes $0, and the rest are $1...
         * The old code set $0 to the shell binary and shifted every operand by
         * one, so `sh -c cmd name a1` gave $0=sh, $1=name, $2=a1 instead of
         * $0=name, $1=a1. modernish's shell probe runs
         * `sh -c '. "$1"...' shellpath std.sh fatal.sh` and expects $1=std.sh;
         * the off-by-one made `. "$1"` try to source the shell binary itself
         * ("ELF: command not found"), so silex was never accepted as a shell. */
        if (arg_start < argc) {
            sh_argc = argc - arg_start;         /* $0 = command_name, $1.. rest */
            sh_argv = malloc((size_t)(sh_argc + 1) * sizeof(char *));
            if (!sh_argv) { perror("sh"); return 1; }
            for (int j = 0; j < sh_argc; j++)
                sh_argv[j] = argv[arg_start + j];
            sh_argv[sh_argc] = NULL;
        } else {
            sh_argc = 1;                        /* no command_name: $0 = shell */
            sh_argv = malloc(2 * sizeof(char *));
            if (!sh_argv) { perror("sh"); return 1; }
            sh_argv[0] = argv[0];
            sh_argv[1] = NULL;
        }
    } else if (arg_start < argc) {
        /* Script mode: argv[arg_start] = $0, rest = $1.. */
        sh_argc = argc - arg_start;
        sh_argv = malloc((size_t)(sh_argc + 1) * sizeof(char *));
        if (!sh_argv) { perror("sh"); return 1; }
        for (int j = 0; j < sh_argc; j++)
            sh_argv[j] = argv[arg_start + j];
        sh_argv[sh_argc] = NULL;
    } else {
        /* stdin mode */
        sh_argc = 1;
        sh_argv = malloc(2 * sizeof(char *));
        if (!sh_argv) { perror("sh"); return 1; }
        sh_argv[0] = argv[0];
        sh_argv[1] = NULL;
    }

    shell_ctx_t sh;
    int rc = shell_init(&sh, sh_argc, sh_argv);
    free(sh_argv);
    if (rc != 0)
        return rc;

    /* Apply parsed flags */
    sh.opt_e        = opt_e;
    sh.opt_u        = opt_u;
    sh.opt_x        = opt_x;
    sh.opt_n        = opt_n;
    sh.opt_f        = opt_f;
    sh.opt_pipefail = opt_pipefail;
    /* -i: interactive semantics regardless of the input source (smoosh
     * sh.ps1.override runs `sh -i <<EOF`): errors that would abort a
     * non-interactive shell only abort the current command, prompts are
     * printed when reading commands, and command lines enter the history. */
    sh.interactive  = opt_i;

    int ret;
    if (cmd_string) {
        ret = shell_run_string(&sh, cmd_string);
    } else if (arg_start < argc) {
        ret = shell_run_file(&sh, argv[arg_start]);
    } else {
        ret = shell_run_stdin(&sh);
    }

    /* Fire EXIT trap on normal script completion. Per the Austin Group
     * resolution smoosh encodes (builtin.trap.subshell.* tests): when the
     * shell terminates NORMALLY (not via the exit builtin), the status of the
     * trap action itself becomes the shell's exit status; an explicit `exit N`
     * inside the action still wins (it exits directly), and a naked `exit`
     * uses the pre-trap status (see exec_builtin_exit). */
    const char *exit_action = sh.traps[0].action;
    if (exit_action != SHELL_TRAP_DEFAULT && exit_action[0] != '\0' &&
        !sh.traps[0].inherited) {
        sh.traps[0].action = SHELL_TRAP_DEFAULT;
        sh.in_trap++;
        sh.trap_entry_status = ret;
        int trap_rc = shell_run_string(&sh, exit_action);
        sh.in_trap--;
        ret = (trap_rc >= 0 && trap_rc <= 255) ? trap_rc : 0;
    }

    shell_free(&sh);
    return ret;
}
