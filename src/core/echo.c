/* echo.c — echo builtin: print arguments to stdout */

#include <stdio.h>
#include <string.h>

/*
 * Parse a single octal escape sequence starting at *p (after the leading \0).
 * Advances *p past the consumed digits (up to 3).
 * Returns the character value.
 */
static unsigned char parse_octal(const char **p)
{
    unsigned val = 0;
    int digits = 0;
    while (digits < 3 && **p >= '0' && **p <= '7') {
        val = val * 8 + (unsigned)(**p - '0');
        (*p)++;
        digits++;
    }
    return (unsigned char)val;
}

/*
 * Parse a hex escape \xHH starting at *p (after the 'x').
 * Advances *p past the consumed digits (up to 2).
 */
static unsigned char parse_hex(const char **p)
{
    unsigned val = 0;
    int digits = 0;
    while (digits < 2) {
        char c = **p;
        if (c >= '0' && c <= '9')      val = val * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (unsigned)(c - 'A' + 10);
        else break;
        (*p)++;
        digits++;
    }
    return (unsigned char)val;
}

/*
 * Print a string with backslash escape interpretation (-e mode).
 * Returns 1 if \c was encountered (suppress further output and newline).
 */
static int print_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s != '\\') {
            putchar(*s);
            continue;
        }
        s++; /* consume backslash */
        switch (*s) {
        case 'a':  putchar('\a'); break;
        case 'b':  putchar('\b'); break;
        case 'c':  return 1; /* suppress rest of output */
        case 'e':  putchar('\033'); break;
        case 'f':  putchar('\f'); break;
        case 'n':  putchar('\n'); break;
        case 'r':  putchar('\r'); break;
        case 't':  putchar('\t'); break;
        case 'v':  putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /*
             * GNU echo -e interprets \NNN (1-3 octal digits starting with
             * any octal digit) as the character with that octal value.
             * parse_octal consumes up to 3 digits from the current position.
             */
            const char *p = s; /* s already points at the first octal digit */
            unsigned char c = parse_octal(&p);
            putchar((int)(unsigned char)c);
            s = p - 1; /* s++ in loop will advance past last consumed digit */
            break;
        }
        case 'x': {
            const char *p = s + 1;
            unsigned char c = parse_hex(&p);
            putchar((int)(unsigned char)c);
            s = p - 1;
            break;
        }
        default:
            /* Unknown escape: print backslash and char literally */
            putchar('\\');
            putchar(*s);
            break;
        }
    }
    return 0;
}

int applet_echo(int argc, char **argv)
{
    int opt_n = 0; /* -n: suppress trailing newline */
    int opt_e = 0; /* -e: interpret backslash escapes */
    int i;

    /*
     * Parse flags.  GNU echo only recognises flags as a leading cluster
     * of -n/-e/-E options.  As soon as we see an argument that is not
     * a recognised flag cluster, everything else is treated as a string.
     */
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Check that every char after '-' is a valid flag */
        const char *p = arg + 1;
        int valid = 1;
        while (*p) {
            if (*p != 'n' && *p != 'e' && *p != 'E') {
                valid = 0;
                break;
            }
            p++;
        }
        if (!valid)
            break;

        /* Apply the flags */
        p = arg + 1;
        while (*p) {
            if (*p == 'n') opt_n = 1;
            else if (*p == 'e') opt_e = 1;
            else if (*p == 'E') opt_e = 0;
            p++;
        }
    }

    /* Print remaining arguments separated by spaces */
    int stopped = 0;
    for (int j = i; j < argc && !stopped; j++) {
        if (j > i)
            putchar(' ');
        if (opt_e)
            stopped = print_escaped(argv[j]);
        else
            fputs(argv[j], stdout);
    }

    if (!opt_n && !stopped)
        putchar('\n');

    return 0;
}
