/* main.c — matchbox multicall entry point: dispatch by argv[0] */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "applets.h"
#include "util/error.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The full applet table.  Terminated by {NULL, NULL, NULL}. */
const applet_t applet_table[] = {
    /* Phase 1 */
    { "cp",       applet_cp,       "cp [-rRpfivLHPantuT] SOURCE... DEST"        },
    { "echo",     applet_echo,     "echo [-neE] [STRING]..."                     },
    { "mkdir",    applet_mkdir,    "mkdir [-pvm MODE] DIR..."                    },
    /* Phase 2: shell */
    { "sh",       applet_sh,       "sh [-ceiuxo] [SCRIPT] [ARG]..."             },
    /* Phase 3: core builtins */
    { "cat",      applet_cat,      "cat [-nbsvAet] [FILE]..."                    },
    { "chmod",    applet_chmod,    "chmod [-Rv] [--reference=RFILE] MODE FILE..."  },
    { "mv",       applet_mv,       "mv [-finuvT] SOURCE... DEST"                  },
    { "rm",       applet_rm,       "rm [-rRfiv] FILE..."                          },
    { "ln",       applet_ln,       "ln [-sfvnrT] TARGET LINK_NAME"               },
    { "touch",    applet_touch,    "touch [-acmt] [-r REF] [-d DATE] FILE..."    },
    { "head",     applet_head,     "head [-n N] [-c N] [-qv] [FILE]..."          },
    { "tail",     applet_tail,     "tail [-n N] [-c N] [-fqv] [FILE]..."         },
    { "wc",       applet_wc,       "wc [-clwmL] [FILE]..."                       },
    { "sort",     applet_sort,     "sort [-bdfginrsu] [-o FILE] [-t SEP] [-k KEY] [FILE]..." },
    { "grep",     applet_grep,     "grep [-EFciln qsvwr] [-e PAT] [-f FILE] PATTERN [FILE]..." },
    { "sed",      applet_sed,      "sed [-nEi] [-e SCRIPT] [-f FILE] [FILE]..."  },
    { "find",     applet_find,     "find [PATH...] [EXPRESSION]"                 },
    { "xargs",    applet_xargs,    "xargs [-0rP N] [-n N] [-I REPL] [-d DELIM] CMD..." },
    { "basename", applet_basename, "basename NAME [SUFFIX] | -a [-s SUFFIX] NAME..." },
    { "dirname",  applet_dirname,  "dirname NAME..."                              },
    { "readlink", applet_readlink, "readlink [-femn qz] FILE..."                  },
    { "stat",     applet_stat,     "stat [-c FORMAT] [-ft] FILE..."              },
    { "date",     applet_date,     "date [-u] [-d DATE] [+FORMAT]"               },
    { "printf",   applet_printf,   "printf FORMAT [ARG]..."                      },
    { "install",  applet_install,  "install [-d] [-m MODE] [-o OWN] [-g GRP] [-vs] SRC DEST" },
    { "tr",       applet_tr,       "tr [-dsc] SET1 [SET2]"                       },
    { "cut",      applet_cut,      "cut [-bcf] [-d DELIM] [-s] FIELDS [FILE]..."  },
    { NULL, NULL, NULL }
};

/* Find an applet by name.  Returns NULL if not found. */
const applet_t *find_applet(const char *name)
{
    for (const applet_t *a = applet_table; a->name; a++) {
        if (strcmp(a->name, name) == 0)
            return a;
    }
    return NULL;
}

/* --list: print all available applets */
static int cmd_list(void)
{
    for (const applet_t *a = applet_table; a->name; a++)
        printf("%s\n", a->name);
    return 0;
}

/*
 * --install DIR: create symlinks DIR/<applet> -> matchbox_path.
 * matchbox_path is the absolute path to the running matchbox binary.
 */
static int cmd_install(const char *dir)
{
    /* Resolve our own executable path */
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) {
        perror("matchbox: readlink /proc/self/exe");
        return 1;
    }
    self[n] = '\0';

    /* Ensure directory exists */
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "matchbox: --install: '%s' is not a directory\n", dir);
        return 1;
    }

    int ret = 0;
    for (const applet_t *a = applet_table; a->name; a++) {
        char link_path[PATH_MAX];
        int r = snprintf(link_path, sizeof(link_path), "%s/%s", dir, a->name);
        if (r < 0 || (size_t)r >= sizeof(link_path)) {
            fprintf(stderr, "matchbox: path too long for symlink %s/%s\n", dir, a->name);
            ret = 1;
            continue;
        }

        /* Remove existing symlink/file */
        if (unlink(link_path) != 0 && errno != ENOENT) {
            perror(link_path);
            ret = 1;
            continue;
        }

        if (symlink(self, link_path) != 0) {
            perror(link_path);
            ret = 1;
        }
    }

    return ret;
}

/* Architecture string for --version */
static const char *matchbox_arch(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "arm";
#elif defined(__i386__)
    return "i386";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#else
    return "unknown";
#endif
}

/* Print help for matchbox itself */
static void print_help(void)
{
    printf("matchbox -- container build runtime\n\n");
    printf("Usage:\n");
    printf("  matchbox --version             Show version\n");
    printf("  matchbox --list                List available applets\n");
    printf("  matchbox --install DIR         Install symlinks in DIR\n");
    printf("  matchbox --help                Show this help\n");
    printf("  <applet> [ARGS...]             Run applet (via argv[0] or symlink)\n\n");
    printf("Applets:\n");
    for (const applet_t *a = applet_table; a->name; a++)
        printf("  %-12s %s\n", a->name, a->usage);
}

int main(int argc, char **argv)
{
    if (argc < 1 || !argv[0])
        return 1;

    /* Determine the invocation name (basename of argv[0]) */
    const char *invname = strrchr(argv[0], '/');
    invname = invname ? invname + 1 : argv[0];

    /* If invoked as "matchbox" directly, handle meta-commands */
    if (strcmp(invname, "matchbox") == 0) {
        if (argc < 2 || strcmp(argv[1], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
#ifndef MATCHBOX_VERSION
#define MATCHBOX_VERSION "unknown"
#endif
            printf("matchbox %s (glibc static-pie, %s, gcc %s)\n",
                   MATCHBOX_VERSION, matchbox_arch(), __VERSION__);
            return 0;
        }
        if (strcmp(argv[1], "--list") == 0)
            return cmd_list();
        if (strcmp(argv[1], "--install") == 0) {
            if (argc < 3) {
                fprintf(stderr, "matchbox: --install requires a directory argument\n");
                return 1;
            }
            return cmd_install(argv[2]);
        }

        /* matchbox <applet> [args]: shift and dispatch */
        const char *applet_name = argv[1];
        const applet_t *a = find_applet(applet_name);
        if (!a) {
            /* Not an applet name: treat as a shell script path or -c string.
             * This allows: matchbox script.sh [args]
             *              matchbox /dev/stdin
             * which is the expected behaviour when matchbox is /bin/sh. */
            const applet_t *sh = find_applet("sh");
            if (sh)
                return sh->fn(argc, argv);
            fprintf(stderr, "matchbox: unknown applet '%s'\n", applet_name);
            fprintf(stderr, "matchbox: run 'matchbox --list' for available applets\n");
            return 1;
        }
        /* Shift argv so applet sees argv[0] = applet name */
        return a->fn(argc - 1, argv + 1);
    }

    /* Invoked via symlink or as a known applet name */
    const applet_t *a = find_applet(invname);
    if (!a) {
        fprintf(stderr, "matchbox: unknown applet '%s'\n", invname);
        return 1;
    }
    return a->fn(argc, argv);
}
