/* sh.c -- sh applet: entry point for the matchbox POSIX shell */

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
 *   -i        interactive (ignored; auto-detected by isatty)
 *   -o OPT    set option by name
 *   --        end options
 */
int applet_sh(int argc, char **argv)
{
    const char *cmd_string = NULL;
    int         arg_start  = argc; /* default: no script file */
    int         opt_e = 0, opt_u = 0, opt_x = 0, opt_n = 0;
    int         opt_f = 0, opt_pipefail = 0;

    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            arg_start = i + 1;
            break;
        }
        if (argv[i][0] != '-') {
            arg_start = i;
            break;
        }
        const char *p = argv[i] + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'c':
                if (*(p + 1)) {
                    cmd_string = p + 1;
                    p = p + strlen(p) - 1; /* skip to end */
                } else if (i + 1 < argc) {
                    cmd_string = argv[++i];
                } else {
                    fprintf(stderr, "matchbox: sh: -c requires an argument\n");
                    return 1;
                }
                arg_start = i + 1;
                stop = 1;
                break;
            case 'e': opt_e        = 1; break;
            case 'u': opt_u        = 1; break;
            case 'x': opt_x        = 1; break;
            case 'n': opt_n        = 1; break;
            case 'f': opt_f        = 1; break;
            case 'i': /* interactive, auto-detected */ break;
            case 'o':
                if (*(p + 1)) {
                    const char *on = p + 1;
                    if      (strcmp(on, "errexit")  == 0) opt_e = 1;
                    else if (strcmp(on, "nounset")  == 0) opt_u = 1;
                    else if (strcmp(on, "xtrace")   == 0) opt_x = 1;
                    else if (strcmp(on, "pipefail") == 0) opt_pipefail = 1;
                    else if (strcmp(on, "noglob")   == 0) opt_f = 1;
                    p = p + strlen(p) - 1;
                } else if (i + 1 < argc) {
                    const char *on = argv[++i];
                    if      (strcmp(on, "errexit")  == 0) opt_e = 1;
                    else if (strcmp(on, "nounset")  == 0) opt_u = 1;
                    else if (strcmp(on, "xtrace")   == 0) opt_x = 1;
                    else if (strcmp(on, "pipefail") == 0) opt_pipefail = 1;
                    else if (strcmp(on, "noglob")   == 0) opt_f = 1;
                    stop = 1;
                }
                break;
            default:
                fprintf(stderr, "matchbox: sh: unknown option: -%c\n", *p);
                return 1;
            }
            if (!stop) p++;
        }
    }

    /* Build positional parameter argv for shell_init.
     * shell_init gets: argv[0] = $0, argv[1..] = $1.. */
    int   sh_argc;
    char **sh_argv;

    if (cmd_string) {
        /* argv[0] is "sh" or the -c arg name; rest are $1.. */
        sh_argc = argc - arg_start + 1;
        if (sh_argc < 1) sh_argc = 1;
        sh_argv = malloc((size_t)(sh_argc + 1) * sizeof(char *));
        if (!sh_argv) { perror("sh"); return 1; }
        sh_argv[0] = argv[0];
        for (int j = 1; j < sh_argc; j++)
            sh_argv[j] = (arg_start + j - 1 < argc) ? argv[arg_start + j - 1] : NULL;
        sh_argv[sh_argc] = NULL;
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

    int ret;
    if (cmd_string) {
        ret = shell_run_string(&sh, cmd_string);
    } else if (arg_start < argc) {
        ret = shell_run_file(&sh, argv[arg_start]);
    } else {
        ret = shell_run_stdin(&sh);
    }

    shell_free(&sh);
    return ret;
}
