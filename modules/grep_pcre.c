#define _POSIX_C_SOURCE 200809L

/*
 * grep_pcre.c — matchbox module stub: grep -P (Perl-compatible regex)
 *
 * This module registers the -P flag for grep.  A full implementation would
 * link against libpcre2-8; here we provide a stub that informs the user that
 * the libpcre2 module is required.
 *
 * To build a real implementation, compile with -lpcre2-8 and replace the
 * handler body with proper PCRE2 matching logic.
 */

#include "../matchbox_module.h"

#include <stdio.h>

static int grep_pcre_handler(int argc, char **argv, int flag_index)
{
    (void)argc;
    (void)argv;
    (void)flag_index;

    fprintf(stderr,
            "matchbox: grep -P requires libpcre2 module\n"
            "  Install the matchbox-pcre2 package or build grep_pcre.so "
            "with -lpcre2-8 to enable Perl-compatible regular expressions.\n");
    return 1;
}

static const char *grep_pcre_flags[] = {
    "-P",
    "--perl-regexp",
    NULL
};

static matchbox_module_t grep_pcre_module = {
    .api_version = MATCHBOX_MODULE_API_VERSION,
    .tool_name   = "grep",
    .module_name = "grep_pcre",
    .description = "grep -P: Perl-compatible regex (requires libpcre2)",
    .extra_flags = grep_pcre_flags,
    .handler     = grep_pcre_handler,
};

matchbox_module_t *matchbox_module_init(void)
{
    return &grep_pcre_module;
}
