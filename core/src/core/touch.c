/* touch.c — touch builtin: change file timestamps */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../cache/fscache.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Parse -t STAMP: [[CC]YY]MMDDhhmm[.ss]
 *
 * Accepted lengths:
 *   8  chars: MMDDhhmm
 *   10 chars: YYMMDDhhmm
 *   12 chars: CCYYMMDDhhmm
 *   10 chars + .ss = 13: YYMMDDhhmm.ss
 *   12 chars + .ss = 15: CCYYMMDDhhmm.ss
 *
 * Returns 0 on success, -1 on parse error.
 * Fills *ts with the parsed time as a CLOCK_REALTIME timespec.
 */
static int parse_t_stamp(const char *s, struct timespec *ts)
{
    size_t len = strlen(s);
    int has_sec = 0;
    int sec = 0;

    /* Detect optional .ss suffix */
    const char *dot = strchr(s, '.');
    if (dot != NULL) {
        /* must be exactly 2 digits after dot */
        const char *p = dot + 1;
        if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] != '\0')
            return -1;
        sec = (p[0] - '0') * 10 + (p[1] - '0');
        if (sec > 60)
            return -1;
        has_sec = 1;
        len = (size_t)(dot - s); /* recount without .ss */
    }

    /* Validate that every char in [s, dot) is a digit */
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
    }

    int CC = -1, YY = -1, MM, DD, hh, mm;

    if (len == 8) {
        /* MMDDhhmm */
        MM = (s[0]-'0')*10 + (s[1]-'0');
        DD = (s[2]-'0')*10 + (s[3]-'0');
        hh = (s[4]-'0')*10 + (s[5]-'0');
        mm = (s[6]-'0')*10 + (s[7]-'0');
    } else if (len == 10) {
        /* YYMMDDhhmm */
        YY = (s[0]-'0')*10 + (s[1]-'0');
        MM = (s[2]-'0')*10 + (s[3]-'0');
        DD = (s[4]-'0')*10 + (s[5]-'0');
        hh = (s[6]-'0')*10 + (s[7]-'0');
        mm = (s[8]-'0')*10 + (s[9]-'0');
    } else if (len == 12) {
        /* CCYYMMDDhhmm */
        CC = (s[0]-'0')*10 + (s[1]-'0');
        YY = (s[2]-'0')*10 + (s[3]-'0');
        MM = (s[4]-'0')*10 + (s[5]-'0');
        DD = (s[6]-'0')*10 + (s[7]-'0');
        hh = (s[8]-'0')*10 + (s[9]-'0');
        mm = (s[10]-'0')*10 + (s[11]-'0');
    } else {
        return -1;
    }

    /* Basic range checks */
    if (MM < 1 || MM > 12) return -1;
    if (DD < 1 || DD > 31) return -1;
    if (hh > 23 || mm > 59) return -1;

    /* Build struct tm */
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec  = has_sec ? sec : 0;
    t.tm_min  = mm;
    t.tm_hour = hh;
    t.tm_mday = DD;
    t.tm_mon  = MM - 1;
    t.tm_isdst = -1;

    if (CC >= 0 && YY >= 0) {
        t.tm_year = CC * 100 + YY - 1900;
    } else if (YY >= 0) {
        /* Two-digit year: 69-99 -> 1969-1999, 00-68 -> 2000-2068 */
        t.tm_year = (YY >= 69) ? (YY) : (YY + 100);
    } else {
        /* No year given: use current year */
        time_t now = time(NULL);
        struct tm *nowtm = localtime(&now);
        if (!nowtm) return -1;
        t.tm_year = nowtm->tm_year;
    }

    time_t result = mktime(&t);
    if (result == (time_t)-1)
        return -1;

    ts->tv_sec  = result;
    ts->tv_nsec = 0;
    return 0;
}

/*
 * Parse -d DATE: simple subset "YYYY-MM-DD [HH:MM:SS]"
 * Returns 0 on success, -1 on parse error.
 */
static int parse_d_date(const char *s, struct timespec *ts)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_isdst = -1;

    int n;
    int yr, mo, dy, hh = 0, mm = 0, ss = 0;

    /* Try with time component first */
    n = sscanf(s, "%d-%d-%d %d:%d:%d", &yr, &mo, &dy, &hh, &mm, &ss);
    if (n != 6 && n != 3)
        return -1;
    if (n == 3) {
        hh = mm = ss = 0;
    }

    if (mo < 1 || mo > 12) return -1;
    if (dy < 1 || dy > 31) return -1;
    if (hh > 23 || mm > 59 || ss > 60) return -1;

    t.tm_year  = yr - 1900;
    t.tm_mon   = mo - 1;
    t.tm_mday  = dy;
    t.tm_hour  = hh;
    t.tm_min   = mm;
    t.tm_sec   = ss;

    time_t result = mktime(&t);
    if (result == (time_t)-1)
        return -1;

    ts->tv_sec  = result;
    ts->tv_nsec = 0;
    return 0;
}

/*
 * Core touch operation for a single path.
 *
 * opt_a:     only update atime
 * opt_m:     only update mtime
 * opt_c:     don't create if not exists
 * have_time: if 1, use *stamp for both (or just the selected one)
 * stamp:     the time to apply (if have_time)
 *
 * Returns 0 on success, 1 on error.
 */
static int touch_path(const char *path,
                      int opt_a, int opt_m, int opt_c,
                      int have_time, const struct timespec *stamp)
{
    /* If file doesn't exist and -c not set, create it */
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno != ENOENT) {
            err_sys("touch", "cannot stat '%s'", path);
            return 1;
        }
        /* Does not exist */
        if (opt_c)
            return 0; /* skip silently */
        int fd = open(path, O_CREAT | O_WRONLY, 0666);
        if (fd < 0) {
            err_sys("touch", "cannot create '%s'", path);
            return 1;
        }
        close(fd);
        /* After creating, fall through to set times */
    }

    struct timespec times[2];

    if (!have_time) {
        /* Use current time */
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            err_sys("touch", "clock_gettime");
            return 1;
        }
        times[0] = now;
        times[1] = now;
    } else {
        times[0] = *stamp;
        times[1] = *stamp;
    }

    /* UTIME_OMIT: don't change if flag not set (when only one side requested) */
    if (!opt_a && opt_m) {
        /* -m only: omit atime */
        times[0].tv_nsec = UTIME_OMIT;
    } else if (opt_a && !opt_m) {
        /* -a only: omit mtime */
        times[1].tv_nsec = UTIME_OMIT;
    }
    /* else: both (or neither means both) */

    if (utimensat(AT_FDCWD, path, times, 0) != 0) {
        err_sys("touch", "setting times on '%s'", path);
        return 1;
    }
    /* B-7: update fscache with fresh stat (written_by_silex=1) */
    {
        struct stat fresh;
        if (stat(path, &fresh) == 0)
            fscache_insert(path, &fresh);
    }
    return 0;
}

int applet_touch(int argc, char **argv)
{
    int opt_a = 0;       /* -a: atime only */
    int opt_m = 0;       /* -m: mtime only */
    int opt_c = 0;       /* -c: no create */
    int have_time = 0;
    struct timespec stamp = {0, 0};
    int ret = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        if (strcmp(arg, "--no-create") == 0)  { opt_c = 1; continue; }

        /* Short flags (may be clustered: -am, -ac, etc.) */
        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'a': opt_a = 1; break;
            case 'm': opt_m = 1; break;
            case 'c': opt_c = 1; break;
            case 't': {
                /* -t STAMP: next arg or remainder */
                const char *stamp_str;
                if (p[1]) {
                    stamp_str = p + 1;
                    stop = 1; /* consume rest of this arg */
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("touch", "[-amc] [-t STAMP] [-r REF] [-d DATE] FILE...");
                        return 1;
                    }
                    stamp_str = argv[i];
                }
                if (parse_t_stamp(stamp_str, &stamp) != 0) {
                    err_msg("touch", "invalid date format: '%s'", stamp_str);
                    return 1;
                }
                have_time = 1;
                stop = 1;
                break;
            }
            case 'r': {
                /* -r REF: use reference file's times */
                const char *ref;
                if (p[1]) {
                    ref = p + 1;
                    stop = 1;
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("touch", "[-amc] [-t STAMP] [-r REF] [-d DATE] FILE...");
                        return 1;
                    }
                    ref = argv[i];
                }
                struct stat ref_st;
                if (stat(ref, &ref_st) != 0) {
                    err_sys("touch", "cannot stat reference '%s'", ref);
                    return 1;
                }
                /* Use mtime of reference as the stamp */
                stamp = ref_st.st_mtim;
                have_time = 1;
                stop = 1;
                break;
            }
            case 'd': {
                /* -d DATE */
                const char *date_str;
                if (p[1]) {
                    date_str = p + 1;
                    stop = 1;
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("touch", "[-amc] [-t STAMP] [-r REF] [-d DATE] FILE...");
                        return 1;
                    }
                    date_str = argv[i];
                }
                if (parse_d_date(date_str, &stamp) != 0) {
                    err_msg("touch", "invalid date string: '%s'", date_str);
                    return 1;
                }
                have_time = 1;
                stop = 1;
                break;
            }
            default:
                err_msg("touch", "unrecognized option '-%c'", *p);
                err_usage("touch", "[-amct] [-t STAMP] [-r REF] [-d DATE] FILE...");
                return 1;
            }
            p++;
        }
    }

    if (i >= argc) {
        err_usage("touch", "[-amct] [-t STAMP] [-r REF] [-d DATE] FILE...");
        return 1;
    }

    for (; i < argc; i++) {
        if (touch_path(argv[i], opt_a, opt_m, opt_c, have_time, &stamp) != 0)
            ret = 1;
    }

    return ret;
}
