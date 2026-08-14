/* printf.c — printf builtin: format and print data */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Escape processing (%b)                                               */
/* ------------------------------------------------------------------ */

/*
 * Parse a \NNN octal escape at *p (p points at first digit after '\').
 * Advances *p past consumed digits (up to 3).
 */
static unsigned char parse_octal_b(const char **p)
{
    unsigned val = 0;
    int d = 0;
    while (d < 3 && **p >= '0' && **p <= '7') {
        val = val * 8 + (unsigned)(**p - '0');
        (*p)++;
        d++;
    }
    return (unsigned char)val;
}

/*
 * Parse a \xHH hex escape at *p (p points at first hex char after 'x').
 * Advances *p past consumed digits (up to 2).
 */
static unsigned char parse_hex_b(const char **p)
{
    unsigned val = 0;
    int d = 0;
    while (d < 2) {
        char c = **p;
        if (c >= '0' && c <= '9')      val = val * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (unsigned)(c - 'A' + 10);
        else break;
        (*p)++;
        d++;
    }
    return (unsigned char)val;
}

/*
 * Print string s with %b escape processing.
 * Returns 1 if \c was encountered (stop all output), 0 otherwise.
 */
static int print_b_string(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p != '\\') {
            putchar(*p);
            continue;
        }
        p++;
        switch (*p) {
        case 'a':  putchar('\a'); break;
        case 'b':  putchar('\b'); break;
        case 'c':  return 1; /* stop output */
        case 'e':  putchar('\033'); break;
        case 'f':  putchar('\f'); break;
        case 'n':  putchar('\n'); break;
        case 'r':  putchar('\r'); break;
        case 't':  putchar('\t'); break;
        case 'v':  putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0': {
            /* \0NNN — octal, up to 3 digits after '0' */
            const char *q = p + 1;
            unsigned char c = parse_octal_b(&q);
            putchar((int)(unsigned char)c);
            p = q - 1;
            break;
        }
        case 'x': {
            const char *q = p + 1;
            unsigned char c = parse_hex_b(&q);
            putchar((int)(unsigned char)c);
            p = q - 1;
            break;
        }
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': {
            /* \NNN where N is 1-7 (non-zero start) */
            const char *q = p;
            unsigned char c = parse_octal_b(&q);
            putchar((int)(unsigned char)c);
            p = q - 1;
            break;
        }
        default:
            putchar('\\');
            putchar(*p);
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shell quoting (%q)                                                    */
/* ------------------------------------------------------------------ */

/*
 * Print s with shell quoting: wrap in single quotes, escaping internal
 * single quotes as '\''.
 */
static void print_q_string(const char *s)
{
    /* Check if empty — needs quoting */
    if (*s == '\0') {
        fputs("''", stdout);
        return;
    }

    /* Check if all chars are safe (alnum, _, -, ., /) */
    int needs_quoting = 0;
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) &&
            *p != '_' && *p != '-' && *p != '.' && *p != '/') {
            needs_quoting = 1;
            break;
        }
    }

    if (!needs_quoting) {
        fputs(s, stdout);
        return;
    }

    putchar('\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            /* End current quote, output escaped single quote, resume */
            fputs("'\\''", stdout);
        } else {
            putchar(*p);
        }
    }
    putchar('\'');
}

/* ------------------------------------------------------------------ */
/* Convert argument to number                                           */
/* ------------------------------------------------------------------ */

/*
 * Parse an argument to a numeric format spec.
 * Handles decimal, octal (0NNN), hex (0xHH), and character ('X).
 */
/* Set when a conversion argument was not a valid number. POSIX: printf
 * still writes a best-effort value but exits non-zero, so `printf %d 3abc`
 * prints 3 AND fails -- silex used to warn and exit 0, which hid the error
 * from every `set -e` script. */
static int g_printf_argerr;

/* Report a bad numeric operand the way dash does; the caller keeps the
 * partially converted value. */
static void bad_number(const char *s, int partial)
{
    if (partial)
        fprintf(stderr, "silex: printf: %s: not completely converted\n", s);
    else
        fprintf(stderr, "silex: printf: %s: expected numeric value\n", s);
    g_printf_argerr = 1;
}

static long long arg_to_llong(const char *s)
{
    if (!s || *s == '\0') return 0;
    if (s[0] == '\'' || s[0] == '"') {
        /* Character constant: value of next character */
        return (long long)(unsigned char)s[1];
    }
    char *endp;
    errno = 0;
    long long val = strtoll(s, &endp, 0);
    if (endp == s) {
        /* Try as unsigned (e.g. a value above LLONG_MAX) */
        unsigned long long uval = strtoull(s, &endp, 0);
        if (endp != s && *endp == '\0')
            return (long long)uval;
        bad_number(s, 0);
        return 0;
    }
    /* Trailing garbage -- including a trailing blank -- is an error, but the
     * converted prefix is still used (dash). Leading blanks are fine. */
    if (*endp != '\0')
        bad_number(s, 1);
    return val;
}

static unsigned long long arg_to_ullong(const char *s)
{
    if (!s || *s == '\0') return 0;
    if (s[0] == '\'' || s[0] == '"')
        return (unsigned long long)(unsigned char)s[1];
    char *endp;
    errno = 0;
    unsigned long long val = strtoull(s, &endp, 0);
    if (endp == s) {
        bad_number(s, 0);
        return 0;
    }
    if (*endp != '\0')
        bad_number(s, 1);
    return val;
}

static double arg_to_double(const char *s)
{
    if (!s || *s == '\0') return 0.0;
    if (s[0] == '\'' || s[0] == '"')
        return (double)(unsigned char)s[1];
    char *endp;
    double val = strtod(s, &endp);
    if (endp == s) {
        bad_number(s, 0);
        return 0.0;
    }
    if (*endp != '\0')
        bad_number(s, 1);
    return val;
}

/* ------------------------------------------------------------------ */
/* Process one format string pass with argv[arg_start..argc-1]         */
/* ------------------------------------------------------------------ */

/*
 * Process the format string once, consuming arguments starting at *arg_idx.
 * *arg_idx is updated.  Returns 1 if \c was seen (stop all output), 0 otherwise.
 */
static int process_format(const char *fmt, int argc, char **argv,
                            int *arg_idx)
{
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'a':  putchar('\a'); break;
            case 'b':  putchar('\b'); break;
            case 'c':  return 1;
            case 'e':  putchar('\033'); break;
            case 'f':  putchar('\f'); break;
            case 'n':  putchar('\n'); break;
            case 'r':  putchar('\r'); break;
            case 't':  putchar('\t'); break;
            case 'v':  putchar('\v'); break;
            case '\\': putchar('\\'); break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                const char *q = p;
                unsigned char c = parse_octal_b(&q);
                putchar((int)(unsigned char)c);
                p = q - 1;
                break;
            }
            case 'x': {
                const char *q = p + 1;
                unsigned char c = parse_hex_b(&q);
                putchar((int)(unsigned char)c);
                p = q - 1;
                break;
            }
            case '"':  putchar('"'); break;
            case '\'': putchar('\''); break;
            case '\0':
                putchar('\\');
                p--;
                break;
            default:
                putchar('\\');
                putchar(*p);
                break;
            }
            continue;
        }

        if (*p != '%') {
            putchar(*p);
            continue;
        }

        p++; /* skip '%' */
        if (*p == '\0') break;

        if (*p == '%') {
            putchar('%');
            continue;
        }

        /* Build a format spec to pass to printf family */
        char spec[64];
        int  si = 0;
        spec[si++] = '%';

        /* Flags: -, +, space, #, 0 */
        while (*p == '-' || *p == '+' || *p == ' ' ||
               *p == '#' || *p == '0') {
            if (si < (int)sizeof(spec) - 5) spec[si++] = *p;
            p++;
        }

        /* Width -- either digits or `*`, which takes the value from the next
         * ARGUMENT (`printf '[%*.*s]' 9 3 hello`). The number is spliced into
         * the spec, so the C library never sees a `*` and needs no va_list. */
        if (*p == '*') {
            long long w = arg_to_llong((*arg_idx < argc) ? argv[(*arg_idx)++] : "");
            /* A negative width means left-justify, exactly like the `-` flag */
            si += snprintf(spec + si, sizeof(spec) - (size_t)si - 5, "%lld", w);
            if (si > (int)sizeof(spec) - 5) si = (int)sizeof(spec) - 5;
            p++;
        } else {
            while (isdigit((unsigned char)*p)) {
                if (si < (int)sizeof(spec) - 5) spec[si++] = *p;
                p++;
            }
        }

        /* Precision */
        if (*p == '.') {
            if (si < (int)sizeof(spec) - 5) spec[si++] = '.';
            p++;
            if (*p == '*') {
                long long pr = arg_to_llong((*arg_idx < argc) ? argv[(*arg_idx)++] : "");
                si += snprintf(spec + si, sizeof(spec) - (size_t)si - 5, "%lld", pr);
                if (si > (int)sizeof(spec) - 5) si = (int)sizeof(spec) - 5;
                p++;
            } else {
                while (isdigit((unsigned char)*p)) {
                    if (si < (int)sizeof(spec) - 5) spec[si++] = *p;
                    p++;
                }
            }
        }

        const char *arg = (*arg_idx < argc) ? argv[(*arg_idx)++] : "";

        switch (*p) {
        case 'd': case 'i': {
            long long val = arg_to_llong(arg);
            spec[si++] = 'l'; spec[si++] = 'l';
            spec[si++] = *p; spec[si] = '\0';
            printf(spec, val); /* NOLINT(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            unsigned long long val = arg_to_ullong(arg);
            spec[si++] = 'l'; spec[si++] = 'l';
            spec[si++] = *p; spec[si] = '\0';
            printf(spec, val);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double val = arg_to_double(arg);
            spec[si++] = *p; spec[si] = '\0';
            printf(spec, val);
            break;
        }
        case 'c': {
            unsigned char c = (arg[0] != '\0') ? (unsigned char)arg[0] : 0;
            spec[si++] = 'c'; spec[si] = '\0';
            printf(spec, (int)c);
            break;
        }
        case 's': {
            spec[si++] = 's'; spec[si] = '\0';
            printf(spec, arg);
            break;
        }
        case 'b': {
            /* %b: like 's' but with escape processing; ignore width/prec
             * for simplicity (GNU behaviour for %b ignores them too) */
            if (print_b_string(arg))
                return 1;
            break;
        }
        case 'q': {
            /* %q: shell-quoted string */
            print_q_string(arg);
            break;
        }
        default:
            /* Unknown: print literally */
            putchar('%');
            /* Re-emit the spec chars we consumed */
            for (int k = 1; k < si; k++) putchar(spec[k]);
            putchar(*p);
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */

int applet_printf(int argc, char **argv)
{
    g_printf_argerr = 0;
    if (argc < 2) {
        /* dash exits 2 for a usage error */
        fprintf(stderr, "silex: printf: usage: printf format [arg ...]\n");
        return 2;
    }

    /* `--` ends the options, so the FORMAT is the word after it. Without
     * this, `printf -- '-%s-\n' a b` took "--" itself as the format and
     * printed a bare "--", swallowing every argument -- and `printf --` is
     * how a script writes a format that begins with a dash. Only argv[1] is
     * considered: a later "--" is an ordinary argument (`printf '%s\n' -- a`
     * prints "--" then "a"). A lone "-" is a format, not an option. */
    int fmt_idx = 1;
    if (strcmp(argv[1], "--") == 0) {
        if (argc < 3) {
            fprintf(stderr, "silex: printf: usage: printf format [arg ...]\n");
            return 2;
        }
        fmt_idx = 2;
    }

    const char *fmt = argv[fmt_idx];
    int first_arg = fmt_idx + 1;

    /* If there are no extra arguments, run once */
    if (first_arg >= argc) {
        int arg_idx = first_arg;
        process_format(fmt, argc, argv, &arg_idx);
        return g_printf_argerr;
    }

    /*
     * GNU behaviour: repeat FORMAT until all arguments are consumed.
     * If FORMAT consumes no arguments, run once to avoid infinite loop.
     */
    int arg_idx = first_arg;
    while (arg_idx < argc) {
        int prev = arg_idx;
        if (process_format(fmt, argc, argv, &arg_idx))
            break; /* \c seen */
        if (arg_idx == prev) {
            /* No argument was consumed this pass — run once and stop */
            break;
        }
    }

    return g_printf_argerr;
}
