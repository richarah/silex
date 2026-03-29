#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* date.c — date builtin */

#include "../util/error.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Weekday helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *const WEEKDAY_NAMES[] = {
    "sunday", "monday", "tuesday", "wednesday",
    "thursday", "friday", "saturday", NULL
};

/* Return 0-6 (Sun-Sat) for name, or -1 if not recognised. */
static int parse_weekday(const char *name)
{
    for (int i = 0; WEEKDAY_NAMES[i]; i++) {
        if (strcasecmp(name, WEEKDAY_NAMES[i]) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Relative date parser                                                  */
/* ------------------------------------------------------------------ */

/*
 * Parse a --date=STRING into a time_t.
 * Supported formats:
 *   @UNIX_TIMESTAMP
 *   YYYY-MM-DD
 *   HH:MM:SS
 *   YYYY-MM-DD HH:MM:SS
 *   today / yesterday / tomorrow
 *   N days ago / N hours ago / N weeks ago / N minutes ago / N seconds ago
 *   next WEEKDAY / last WEEKDAY
 *   now
 * Returns 0 on success, -1 on failure.
 */
static int parse_date_string(const char *s, time_t *out)
{
    /* Unix timestamp */
    if (s[0] == '@') {
        char *endp;
        long long val = strtoll(s + 1, &endp, 10);
        if (endp == s + 1 || (*endp != '\0' && !isspace((unsigned char)*endp)))
            return -1;
        *out = (time_t)val;
        return 0;
    }

    /* Grab current local time */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* today / now */
    if (strcasecmp(s, "today") == 0 || strcasecmp(s, "now") == 0) {
        *out = now;
        return 0;
    }

    /* yesterday */
    if (strcasecmp(s, "yesterday") == 0) {
        *out = now - 86400;
        return 0;
    }

    /* tomorrow */
    if (strcasecmp(s, "tomorrow") == 0) {
        *out = now + 86400;
        return 0;
    }

    /* N unit ago */
    {
        long n;
        char unit[32];
        char ago[8];
        if (sscanf(s, "%ld %31s %7s", &n, unit, ago) == 3 &&
            strcasecmp(ago, "ago") == 0) {
            long secs = 0;
            if (strcasecmp(unit, "second") == 0 ||
                strcasecmp(unit, "seconds") == 0)  secs = n;
            else if (strcasecmp(unit, "minute") == 0 ||
                     strcasecmp(unit, "minutes") == 0) secs = n * 60;
            else if (strcasecmp(unit, "hour") == 0 ||
                     strcasecmp(unit, "hours") == 0)   secs = n * 3600;
            else if (strcasecmp(unit, "day") == 0 ||
                     strcasecmp(unit, "days") == 0)    secs = n * 86400;
            else if (strcasecmp(unit, "week") == 0 ||
                     strcasecmp(unit, "weeks") == 0)   secs = n * 604800;
            else return -1;
            *out = now - secs;
            return 0;
        }
    }

    /* N unit (forward) */
    {
        long n;
        char unit[32];
        if (sscanf(s, "%ld %31s", &n, unit) == 2) {
            /* Make sure there's no 3rd token (would be "ago") */
            char extra[8];
            if (sscanf(s, "%ld %31s %7s", &n, unit, extra) != 3) {
                long secs = 0;
                int matched = 1;
                if (strcasecmp(unit, "second") == 0 ||
                    strcasecmp(unit, "seconds") == 0)  secs = n;
                else if (strcasecmp(unit, "minute") == 0 ||
                         strcasecmp(unit, "minutes") == 0) secs = n * 60;
                else if (strcasecmp(unit, "hour") == 0 ||
                         strcasecmp(unit, "hours") == 0)   secs = n * 3600;
                else if (strcasecmp(unit, "day") == 0 ||
                         strcasecmp(unit, "days") == 0)    secs = n * 86400;
                else if (strcasecmp(unit, "week") == 0 ||
                         strcasecmp(unit, "weeks") == 0)   secs = n * 604800;
                else matched = 0;

                if (matched) {
                    *out = now + secs;
                    return 0;
                }
            }
        }
    }

    /* next WEEKDAY */
    if (strncasecmp(s, "next ", 5) == 0) {
        int wday = parse_weekday(s + 5);
        if (wday < 0) return -1;
        int diff = wday - tm_now.tm_wday;
        if (diff <= 0) diff += 7;
        *out = now + diff * 86400;
        return 0;
    }

    /* last WEEKDAY */
    if (strncasecmp(s, "last ", 5) == 0) {
        int wday = parse_weekday(s + 5);
        if (wday < 0) return -1;
        int diff = tm_now.tm_wday - wday;
        if (diff <= 0) diff += 7;
        *out = now - diff * 86400;
        return 0;
    }

    /* YYYY-MM-DD HH:MM:SS */
    {
        struct tm t = {0};
        int n_matched = sscanf(s, "%d-%d-%d %d:%d:%d",
                                &t.tm_year, &t.tm_mon, &t.tm_mday,
                                &t.tm_hour, &t.tm_min, &t.tm_sec);
        if (n_matched == 6) {
            t.tm_year -= 1900;
            t.tm_mon  -= 1;
            t.tm_isdst = -1;
            *out = mktime(&t);
            if (*out == (time_t)-1) return -1;
            return 0;
        }
    }

    /* YYYY-MM-DD */
    {
        struct tm t = {0};
        int n_matched = sscanf(s, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
        if (n_matched == 3) {
            t.tm_year -= 1900;
            t.tm_mon  -= 1;
            t.tm_isdst = -1;
            *out = mktime(&t);
            if (*out == (time_t)-1) return -1;
            return 0;
        }
    }

    /* HH:MM:SS */
    {
        struct tm t = tm_now; /* start from today */
        int h, m, sec;
        if (sscanf(s, "%d:%d:%d", &h, &m, &sec) == 3) {
            t.tm_hour = h;
            t.tm_min  = m;
            t.tm_sec  = sec;
            t.tm_isdst = -1;
            *out = mktime(&t);
            if (*out == (time_t)-1) return -1;
            return 0;
        }
    }

    return -1; /* unrecognised */
}

/* ------------------------------------------------------------------ */
/* RFC 2822 formatting                                                   */
/* ------------------------------------------------------------------ */

static void format_rfc2822(const struct tm *tm_val, char *buf, size_t bufsz)
{
    /* Example: Wed, 01 Jan 2025 00:00:00 +0000 */
    strftime(buf, bufsz, "%a, %d %b %Y %H:%M:%S %z", tm_val);
}

/* ------------------------------------------------------------------ */
/* ISO 8601 formatting                                                   */
/* ------------------------------------------------------------------ */

static void format_iso8601(const struct tm *tm_val, const char *timespec,
                             char *buf, size_t bufsz)
{
    /* timespec: "date" | "hours" | "minutes" | "seconds" | "ns" */
    if (!timespec || strcasecmp(timespec, "date") == 0) {
        strftime(buf, bufsz, "%Y-%m-%d", tm_val);
        return;
    }
    if (strcasecmp(timespec, "hours") == 0) {
        strftime(buf, bufsz, "%Y-%m-%dT%H%z", tm_val);
        return;
    }
    if (strcasecmp(timespec, "minutes") == 0) {
        strftime(buf, bufsz, "%Y-%m-%dT%H:%M%z", tm_val);
        return;
    }
    /* "seconds" or default */
    strftime(buf, bufsz, "%Y-%m-%dT%H:%M:%S%z", tm_val);
}

/* ------------------------------------------------------------------ */
/* Main                                                                  */
/* ------------------------------------------------------------------ */

int applet_date(int argc, char **argv)
{
    int opt_u    = 0;   /* -u: UTC */
    int opt_R    = 0;   /* -R: RFC 2822 */
    int opt_set  = 0;   /* -s: set time */
    const char *opt_date   = NULL;  /* -d / --date= */
    const char *opt_format = NULL;  /* +FORMAT */
    const char *opt_iso    = NULL;  /* -I[TIMESPEC] */
    const char *set_date   = NULL;  /* -s DATE */
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        /* +FORMAT */
        if (arg[0] == '+') {
            opt_format = arg + 1;
            continue;
        }

        if (strncmp(arg, "--date=", 7) == 0) {
            opt_date = arg + 7;
            continue;
        }
        if (strcmp(arg, "--date") == 0 || strcmp(arg, "-d") == 0) {
            i++;
            if (i >= argc) {
                err_usage("date", "[-uR] [-d STRING] [+FORMAT]");
                return 1;
            }
            opt_date = argv[i];
            continue;
        }
        if (strcmp(arg, "--utc") == 0 || strcmp(arg, "--universal") == 0) {
            opt_u = 1;
            continue;
        }
        if (strcmp(arg, "--rfc-2822") == 0 ||
            strcmp(arg, "--rfc-email") == 0) {
            opt_R = 1;
            continue;
        }
        if (strncmp(arg, "--iso-8601", 10) == 0) {
            if (arg[10] == '=')
                opt_iso = arg + 11;
            else
                opt_iso = "seconds";
            continue;
        }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'u': opt_u = 1; break;
            case 'R': opt_R = 1; break;
            case 'I':
                /* -I or -ITIMESPEC */
                if (p[1]) {
                    opt_iso = p + 1;
                    stop = 1;
                } else {
                    opt_iso = "seconds";
                }
                break;
            case 's':
                /* -s DATE */
                if (p[1]) {
                    set_date = p + 1;
                    stop = 1;
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("date", "[-uR] [-d STRING] [-s DATE] [+FORMAT]");
                        return 1;
                    }
                    set_date = argv[i];
                }
                opt_set = 1;
                break;
            default:
                err_msg("date", "unrecognized option '-%c'", *p);
                err_usage("date", "[-uRIs] [-d STRING] [+FORMAT]");
                return 1;
            }
            p++;
        }
    }

    /* Determine the time to display */
    time_t t;
    if (opt_date) {
        if (parse_date_string(opt_date, &t) != 0) {
            err_msg("date", "invalid date '%s'", opt_date);
            return 1;
        }
    } else {
        t = time(NULL);
    }

    /* Set system time (-s) */
    if (opt_set && set_date) {
        time_t set_t;
        if (parse_date_string(set_date, &set_t) != 0) {
            err_msg("date", "invalid date '%s'", set_date);
            return 1;
        }
        struct timespec ts = { set_t, 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            err_sys("date", "cannot set time");
            return 1;
        }
        t = set_t;
    }

    /* Convert to tm */
    struct tm tm_val;
    if (opt_u)
        gmtime_r(&t, &tm_val);
    else
        localtime_r(&t, &tm_val);

    char buf[256];

    if (opt_R) {
        format_rfc2822(&tm_val, buf, sizeof(buf));
        puts(buf);
        return 0;
    }

    if (opt_iso) {
        format_iso8601(&tm_val, opt_iso, buf, sizeof(buf));
        puts(buf);
        return 0;
    }

    if (opt_format) {
        size_t n = strftime(buf, sizeof(buf), opt_format, &tm_val);
        if (n == 0 && opt_format[0] != '\0') {
            /* Buffer too small — try with heap allocation */
            size_t sz = 4096;
            char *hbuf = malloc(sz);
            if (!hbuf) {
                err_msg("date", "out of memory");
                return 1;
            }
            n = strftime(hbuf, sz, opt_format, &tm_val);
            if (n == 0) {
                free(hbuf);
                err_msg("date", "format string produced no output");
                return 1;
            }
            puts(hbuf);
            free(hbuf);
            return 0;
        }
        puts(buf);
        return 0;
    }

    /* Default: "%a %b %e %H:%M:%S %Z %Y" */
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm_val);
    puts(buf);
    return 0;
}
