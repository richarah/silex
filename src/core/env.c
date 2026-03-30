/* env.c — env builtin: print/modify environment and run commands */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declaration for environ */
extern char **environ;

int applet_env(int argc, char **argv)
{
    int opt_i    = 0;   /* -i: start with empty environment */
    int i;

    /* Collect -u VAR unset requests */
    const char *unset_vars[256];
    int nunset = 0;

    /* Collect VAR=val additions */
    const char *set_vars[256];
    int nset = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        /* -i */
        if (strcmp(arg, "-i") == 0) { opt_i = 1; continue; }

        /* -u VAR */
        if (strcmp(arg, "-u") == 0) {
            i++;
            if (i >= argc) {
                err_usage("env", "[-i] [-u NAME] [NAME=VALUE]... [CMD [ARG]...]");
                return 1;
            }
            if (nunset < 256) unset_vars[nunset++] = argv[i];
            continue;
        }
        /* --unset=VAR */
        if (strncmp(arg, "--unset=", 8) == 0) {
            if (nunset < 256) unset_vars[nunset++] = arg + 8;
            continue;
        }

        /* Long option -u VAR as -uVAR */
        if (arg[0] == '-' && arg[1] == 'u' && arg[2] != '\0') {
            if (nunset < 256) unset_vars[nunset++] = arg + 2;
            continue;
        }

        err_msg("env", "unrecognized option '%s'", arg);
        err_usage("env", "[-i] [-u NAME] [NAME=VALUE]... [CMD [ARG]...]");
        return 1;
    }

    /* Collect NAME=VALUE pairs before command */
    int cmd_start = i;
    for (; cmd_start < argc; cmd_start++) {
        const char *arg = argv[cmd_start];
        /* Is it a NAME=VALUE assignment? */
        const char *eq = strchr(arg, '=');
        if (!eq) break;
        /* Validate: NAME must be a valid identifier (letters, digits, underscore) */
        int valid = 1;
        for (const char *p = arg; p < eq; p++) {
            char c = *p;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_')) {
                valid = 0; break;
            }
        }
        if (!valid) break;
        if (nset < 256) set_vars[nset++] = arg;
    }

    /* Build new environment for exec case */
    char **new_env = NULL;
    int env_count  = 0;

    if (cmd_start < argc) {
        /* We'll exec a command — build environment */
        if (opt_i) {
            /* Start empty: only add set_vars */
            new_env = malloc(((size_t)nset + 1) * sizeof(char *));
            if (!new_env) { err_msg("env", "out of memory"); return 1; }
            for (int k = 0; k < nset; k++)
                new_env[env_count++] = (char *)set_vars[k];
            new_env[env_count] = NULL;
        } else {
            /* Count current env */
            int cur_count = 0;
            if (environ) {
                for (; environ[cur_count]; cur_count++) {}
            }
            /* Max size: cur + new */
            new_env = malloc(((size_t)(cur_count + nset + 1)) * sizeof(char *));
            if (!new_env) { err_msg("env", "out of memory"); return 1; }

            /* Copy existing entries, skipping unset and overridden */
            for (int k = 0; environ && environ[k]; k++) {
                /* Check if this var is being unset */
                int skip = 0;
                const char *eq2 = strchr(environ[k], '=');
                size_t varlen = eq2 ? (size_t)(eq2 - environ[k]) : strlen(environ[k]);

                for (int u = 0; u < nunset && !skip; u++) {
                    size_t ulen = strlen(unset_vars[u]);
                    if (ulen == varlen &&
                        strncmp(environ[k], unset_vars[u], varlen) == 0)
                        skip = 1;
                }
                /* Check if being overridden by set_vars */
                for (int s = 0; s < nset && !skip; s++) {
                    const char *eq3 = strchr(set_vars[s], '=');
                    size_t slen = eq3 ? (size_t)(eq3 - set_vars[s]) : strlen(set_vars[s]);
                    if (slen == varlen &&
                        strncmp(environ[k], set_vars[s], varlen) == 0)
                        skip = 1;
                }
                if (!skip)
                    new_env[env_count++] = environ[k];
            }
            /* Append set_vars */
            for (int s = 0; s < nset; s++)
                new_env[env_count++] = (char *)set_vars[s];
            new_env[env_count] = NULL;
        }

        /* If the command contains '/', exec directly; otherwise search PATH */
        if (strchr(argv[cmd_start], '/')) {
            execve(argv[cmd_start], argv + cmd_start, new_env);
            err_msg("env", "%s: %s", argv[cmd_start], strerror(errno));
            free(new_env);
            return 127;
        }

        /* Search PATH from the new environment */
        const char *path = "/usr/local/bin:/usr/bin:/bin";
        for (int k = 0; new_env[k]; k++) {
            if (strncmp(new_env[k], "PATH=", 5) == 0) {
                path = new_env[k] + 5;
                break;
            }
        }

        char exe[4096];
        const char *p_start = path;
        while (p_start && *p_start) {
            const char *p_end = strchr(p_start, ':');
            size_t dirlen = p_end ? (size_t)(p_end - p_start) : strlen(p_start);
            if (dirlen + 1 + strlen(argv[cmd_start]) + 1 < sizeof(exe)) {
                memcpy(exe, p_start, dirlen);
                exe[dirlen] = '/';
                strcpy(exe + dirlen + 1, argv[cmd_start]);
                execve(exe, argv + cmd_start, new_env);
                if (errno != ENOENT && errno != EACCES) break;
            }
            p_start = p_end ? p_end + 1 : NULL;
        }
        err_msg("env", "%s: command not found", argv[cmd_start]);
        free(new_env);
        return 127;
    }

    /* No command: print environment */
    if (opt_i && nset == 0) {
        /* Nothing to print */
        return 0;
    }

    if (opt_i) {
        for (int s = 0; s < nset; s++)
            puts(set_vars[s]);
        return 0;
    }

    /* Print current environment, applying -u filters and additions */
    for (int k = 0; environ && environ[k]; k++) {
        int skip = 0;
        const char *eq2 = strchr(environ[k], '=');
        size_t varlen = eq2 ? (size_t)(eq2 - environ[k]) : strlen(environ[k]);
        for (int u = 0; u < nunset && !skip; u++) {
            size_t ulen = strlen(unset_vars[u]);
            if (ulen == varlen && strncmp(environ[k], unset_vars[u], varlen) == 0)
                skip = 1;
        }
        if (!skip) puts(environ[k]);
    }
    /* Print any additions */
    for (int s = 0; s < nset; s++)
        puts(set_vars[s]);

    return 0;
}
