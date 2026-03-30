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
    if (errno != 0 || endp == s) {
        /* Try as unsigned */
        unsigned long long uval = strtoull(s, &endp, 0);
        if (endp != s)
            return (long long)uval;
        /* Non-numeric: return 0, print warning */
        fprintf(stderr, "matchbox: printf: '%s': invalid number\n", s);
        return 0;
    }
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
    if (errno != 0 || endp == s) {
        fprintf(stderr, "matchbox: printf: '%s': invalid number\n", s);
        return 0;
    }
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
        fprintf(stderr, "matchbox: printf: '%s': invalid number\n", s);
        return 0.0;
    }
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

        /* Width */
        while (isdigit((unsigned char)*p)) {
            if (si < (int)sizeof(spec) - 5) spec[si++] = *p;
            p++;
        }

        /* Precision */
        if (*p == '.') {
            if (si < (int)sizeof(spec) - 5) spec[si++] = '.';
            p++;
            while (isdigit((unsigned char)*p)) {
                if (si < (int)sizeof(spec) - 5) spec[si++] = *p;
                p++;
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
    if (argc < 2) {
        err_usage("printf", "FORMAT [ARG...]");
        return 1;
    }

    const char *fmt = argv[1];
    int first_arg = 2;

    /* If there are no extra arguments, run once */
    if (first_arg >= argc) {
        int arg_idx = first_arg;
        process_format(fmt, argc, argv, &arg_idx);
        return 0;
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

    return 0;
}
