/* error.c — error reporting utilities */

#include "error.h"
#include "section.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

COLD void err_msg(const char *name, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "matchbox");
    if (name && *name)
        fprintf(stderr, ": %s", name);
    fprintf(stderr, ": ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

COLD void err_sys(const char *name, const char *fmt, ...)
{
    int saved = errno;
    va_list ap;
    fprintf(stderr, "matchbox");
    if (name && *name)
        fprintf(stderr, ": %s", name);
    fprintf(stderr, ": ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(saved));
}

COLD void err_usage(const char *name, const char *usage)
{
    fprintf(stderr, "Usage: %s %s\n", name, usage);
    fprintf(stderr, "Try '%s --help' for more information.\n", name);
}
