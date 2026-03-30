/* stat.c -- stat builtin: display file or file system status */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* stat.c — stat builtin */

#include "../util/error.h"
#include "../util/strbuf.h"

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Return a string describing the file type. */
static const char *file_type_str(mode_t m)
{
    if (S_ISREG(m))  return "regular file";
    if (S_ISDIR(m))  return "directory";
    if (S_ISLNK(m))  return "symbolic link";
    if (S_ISBLK(m))  return "block special file";
    if (S_ISCHR(m))  return "character special file";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

/* Build a symbolic permissions string like "-rwxr-xr-x" into buf (11 bytes + NUL). */
static void sym_perms(mode_t m, char *buf)
{
    /* File type character */
    if      (S_ISREG(m))  buf[0] = '-';
    else if (S_ISDIR(m))  buf[0] = 'd';
    else if (S_ISLNK(m))  buf[0] = 'l';
    else if (S_ISBLK(m))  buf[0] = 'b';
    else if (S_ISCHR(m))  buf[0] = 'c';
    else if (S_ISFIFO(m)) buf[0] = 'p';
    else if (S_ISSOCK(m)) buf[0] = 's';
    else                   buf[0] = '?';

    const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) {
        buf[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    }
    /* Handle setuid/setgid/sticky */
    if (m & S_ISUID) buf[3] = (m & S_IXUSR) ? 's' : 'S';
    if (m & S_ISGID) buf[6] = (m & S_IXGRP) ? 's' : 'S';
#ifdef S_ISVTX
    if (m & S_ISVTX) buf[9] = (m & S_IXOTH) ? 't' : 'T';
#endif
    buf[10] = '\0';
}

/*
 * Format a struct timespec as "YYYY-MM-DD HH:MM:SS.NNNNNNNNN +ZZZZ"
 * into buf (must be at least 40 bytes).
 */
static void format_time(const struct timespec *ts, char *buf, size_t bufsz,
                        int use_utc)
{
    struct tm tm_val;
    if (use_utc)
        gmtime_r(&ts->tv_sec, &tm_val);
    else
        localtime_r(&ts->tv_sec, &tm_val);

    char date_part[32];
    strftime(date_part, sizeof(date_part), "%Y-%m-%d %H:%M:%S", &tm_val);

    char tz_part[8];
    strftime(tz_part, sizeof(tz_part), "%z", &tm_val);

    int n = snprintf(buf, bufsz, "%s.%09ld %s",
                     date_part, (long)ts->tv_nsec, tz_part);
    if (n < 0 || (size_t)n >= bufsz)
        buf[bufsz - 1] = '\0';
}

/* Look up username for uid; returns static buffer or numeric string. */
static const char *uid_name(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (pw) return pw->pw_name;
    static char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u", (unsigned)uid);
    if (n < 0 || (size_t)n >= sizeof(buf)) buf[sizeof(buf)-1] = '\0';
    return buf;
}

/* Look up groupname for gid; returns static buffer or numeric string. */
static const char *gid_name(gid_t gid)
{
    struct group *gr = getgrgid(gid);
    if (gr) return gr->gr_name;
    static char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u", (unsigned)gid);
    if (n < 0 || (size_t)n >= sizeof(buf)) buf[sizeof(buf)-1] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Format string processing for -c FORMAT                              */
/* ------------------------------------------------------------------ */

/*
 * Process one format directive starting at *fmt (the character after '%').
 * Appends the result to sb.  Advances *fmt past the directive.
 * Returns 0 on success, 1 on unrecognised directive.
 */
static int process_directive(const char **fmt, const char *path,
                              const struct stat *st, strbuf_t *sb,
                              int use_utc)
{
    char tmp[256];
    int n;

    switch (**fmt) {
    case 'n': /* file name */
        if (sb_append(sb, path) < 0) return 1;
        break;
    case 'N': /* quoted file name */
        n = snprintf(tmp, sizeof(tmp), "'%s'", path);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 's': /* total size in bytes */
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)st->st_size);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'b': /* number of 512-byte blocks */
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)st->st_blocks);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'B': /* block size (always 512) */
        if (sb_append(sb, "512") < 0) return 1;
        break;
    case 'f': /* raw hex mode */
        n = snprintf(tmp, sizeof(tmp), "%x", (unsigned)st->st_mode);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'F': /* file type string */
        if (sb_append(sb, file_type_str(st->st_mode)) < 0) return 1;
        break;
    case 'u': /* uid */
        n = snprintf(tmp, sizeof(tmp), "%u", (unsigned)st->st_uid);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'g': /* gid */
        n = snprintf(tmp, sizeof(tmp), "%u", (unsigned)st->st_gid);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'U': /* username */
        if (sb_append(sb, uid_name(st->st_uid)) < 0) return 1;
        break;
    case 'G': /* groupname */
        if (sb_append(sb, gid_name(st->st_gid)) < 0) return 1;
        break;
    case 'i': /* inode */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)st->st_ino);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'h': /* hard link count */
        n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)st->st_nlink);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'a': /* octal permissions */
        n = snprintf(tmp, sizeof(tmp), "%o", (unsigned)(st->st_mode & 07777));
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'A': { /* symbolic permissions */
        char perms[12];
        sym_perms(st->st_mode, perms);
        if (sb_append(sb, perms) < 0) return 1;
        break;
    }
    case 'x': { /* atime */
        char tbuf[64];
        format_time(&st->st_atim, tbuf, sizeof(tbuf), use_utc);
        if (sb_append(sb, tbuf) < 0) return 1;
        break;
    }
    case 'y': { /* mtime */
        char tbuf[64];
        format_time(&st->st_mtim, tbuf, sizeof(tbuf), use_utc);
        if (sb_append(sb, tbuf) < 0) return 1;
        break;
    }
    case 'z': { /* ctime */
        char tbuf[64];
        format_time(&st->st_ctim, tbuf, sizeof(tbuf), use_utc);
        if (sb_append(sb, tbuf) < 0) return 1;
        break;
    }
    case 'W': /* birth time — not available in stat on Linux; print 0 */
        if (sb_append(sb, "0") < 0) return 1;
        break;
    case 'X': /* atime seconds */
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)st->st_atim.tv_sec);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'Y': /* mtime seconds */
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)st->st_mtim.tv_sec);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'Z': /* ctime seconds */
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)st->st_ctim.tv_sec);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'd': /* device number (decimal) */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)st->st_dev);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'D': /* device number (hex) */
        n = snprintf(tmp, sizeof(tmp), "%llx", (unsigned long long)st->st_dev);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'r': /* device type if special (decimal) */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)st->st_rdev);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'R': /* device type if special (hex) */
        n = snprintf(tmp, sizeof(tmp), "%llx", (unsigned long long)st->st_rdev);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'o': /* optimal I/O block size */
        n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)st->st_blksize);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case '%':
        if (sb_appendc(sb, '%') < 0) return 1;
        break;
    default:
        /* Unknown directive: print literally */
        if (sb_appendc(sb, '%') < 0) return 1;
        if (sb_appendc(sb, **fmt) < 0) return 1;
        break;
    }
    (*fmt)++;
    return 0;
}

/*
 * Process format directives for filesystem stats (-f flag).
 */
static int process_fs_directive(const char **fmt, const char *path,
                                 const struct statvfs *sv, strbuf_t *sb)
{
    char tmp[256];
    int n;

    switch (**fmt) {
    case 'n': if (sb_append(sb, path) < 0) return 1; break;
    case 'i': /* filesystem id — show as hex pair */
        n = snprintf(tmp, sizeof(tmp), "%lx", (unsigned long)sv->f_fsid);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'l': /* max filename length */
        n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)sv->f_namemax);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 't': /* filesystem type (hex) */
        n = snprintf(tmp, sizeof(tmp), "%lx", (unsigned long)sv->f_flag);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'T': if (sb_append(sb, "unknown") < 0) return 1; break;
    case 'b': /* total blocks */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)sv->f_blocks);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'f': /* free blocks */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)sv->f_bfree);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'a': /* available blocks (non-root) */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)sv->f_bavail);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 's': /* block size */
        n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)sv->f_frsize);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'S': /* fundamental block size */
        n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)sv->f_bsize);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'c': /* total inodes */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)sv->f_files);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case 'd': /* free inodes */
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)sv->f_ffree);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
        if (sb_append(sb, tmp) < 0) return 1;
        break;
    case '%':
        if (sb_appendc(sb, '%') < 0) return 1;
        break;
    default:
        if (sb_appendc(sb, '%') < 0) return 1;
        if (sb_appendc(sb, **fmt) < 0) return 1;
        break;
    }
    (*fmt)++;
    return 0;
}

/* Expand escape sequences (\n, \t, \\, etc.) in a format string. */
static int expand_escapes(const char *in, strbuf_t *sb)
{
    for (const char *p = in; *p; p++) {
        if (*p != '\\') {
            if (sb_appendc(sb, *p) < 0) return -1;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': if (sb_appendc(sb, '\n') < 0) return -1; break;
        case 't': if (sb_appendc(sb, '\t') < 0) return -1; break;
        case '\\': if (sb_appendc(sb, '\\') < 0) return -1; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            unsigned val = (unsigned)(*p - '0');
            int d = 1;
            while (d < 3 && p[1] >= '0' && p[1] <= '7') {
                val = val * 8 + (unsigned)(p[1] - '0');
                p++; d++;
            }
            if (sb_appendc(sb, (char)(unsigned char)val) < 0) return -1;
            break;
        }
        case '\0': /* trailing backslash */
            if (sb_appendc(sb, '\\') < 0) return -1;
            p--;
            break;
        default:
            if (sb_appendc(sb, '\\') < 0) return -1;
            if (sb_appendc(sb, *p) < 0) return -1;
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output one file's default (no format) human-readable output         */
/* ------------------------------------------------------------------ */

static int print_default(const char *path, int opt_fs, int follow_links,
                          int use_utc)
{
    if (opt_fs) {
        struct statvfs sv;
        if (statvfs(path, &sv) != 0) {
            err_sys("stat", "cannot statfs '%s'", path);
            return 1;
        }
        printf("  File: \"%s\"\n", path);
        printf("    ID: %-16lx Namelen: %-7lu Type: unknown\n",
               (unsigned long)sv.f_fsid, (unsigned long)sv.f_namemax);
        printf("Block size: %-10lu Fundamental block size: %lu\n",
               (unsigned long)sv.f_bsize, (unsigned long)sv.f_frsize);
        printf("Blocks: Total: %-10llu Free: %-10llu Available: %llu\n",
               (unsigned long long)sv.f_blocks,
               (unsigned long long)sv.f_bfree,
               (unsigned long long)sv.f_bavail);
        printf("Inodes: Total: %-10llu Free: %llu\n",
               (unsigned long long)sv.f_files,
               (unsigned long long)sv.f_ffree);
        return 0;
    }

    struct stat st;
    int rc = follow_links ? stat(path, &st) : lstat(path, &st);
    if (rc != 0) {
        err_sys("stat", "cannot stat '%s'", path);
        return 1;
    }

    char perms[12];
    sym_perms(st.st_mode, perms);

    char atime[64], mtime[64], ctime[64];
    format_time(&st.st_atim, atime, sizeof(atime), use_utc);
    format_time(&st.st_mtim, mtime, sizeof(mtime), use_utc);
    format_time(&st.st_ctim, ctime, sizeof(ctime), use_utc);

    printf("  File: %s\n", path);
    printf("  Size: %-15lld\tBlocks: %-10lld IO Block: %-6lu %s\n",
           (long long)st.st_size, (long long)st.st_blocks,
           (unsigned long)st.st_blksize, file_type_str(st.st_mode));
    printf("Device: %lluh/%llud\tInode: %-11llu  Links: %lu\n",
           (unsigned long long)st.st_dev, (unsigned long long)st.st_dev,
           (unsigned long long)st.st_ino, (unsigned long)st.st_nlink);
    printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
           (unsigned)(st.st_mode & 07777), perms,
           (unsigned)st.st_uid, uid_name(st.st_uid),
           (unsigned)st.st_gid, gid_name(st.st_gid));
    printf("Access: %s\n", atime);
    printf("Modify: %s\n", mtime);
    printf("Change: %s\n", ctime);
    printf(" Birth: -\n");

    return 0;
}

/* ------------------------------------------------------------------ */
/* Terse output                                                         */
/* ------------------------------------------------------------------ */

static int print_terse(const char *path, int opt_fs, int follow_links,
                        int use_utc)
{
    if (opt_fs) {
        struct statvfs sv;
        if (statvfs(path, &sv) != 0) {
            err_sys("stat", "cannot statfs '%s'", path);
            return 1;
        }
        /* GNU stat -tf terse: name id namelen type bsize bfree bavail files ffree */
        printf("%s %lx %lu %lx %lu %llu %llu %llu %llu\n",
               path,
               (unsigned long)sv.f_fsid,
               (unsigned long)sv.f_namemax,
               (unsigned long)sv.f_flag,
               (unsigned long)sv.f_frsize,
               (unsigned long long)sv.f_blocks,
               (unsigned long long)sv.f_bfree,
               (unsigned long long)sv.f_bavail,
               (unsigned long long)sv.f_files);
        return 0;
    }

    struct stat st;
    int rc = follow_links ? stat(path, &st) : lstat(path, &st);
    if (rc != 0) {
        err_sys("stat", "cannot stat '%s'", path);
        return 1;
    }

    (void)use_utc; /* timestamps as raw seconds in terse mode */
    printf("%s %lld %lld %x %u %u %llu %llu %lu %lld %lld %lld %lld\n",
           path,
           (long long)st.st_size,
           (long long)st.st_blocks,
           (unsigned)st.st_mode,
           (unsigned)st.st_uid,
           (unsigned)st.st_gid,
           (unsigned long long)st.st_dev,
           (unsigned long long)st.st_ino,
           (unsigned long)st.st_nlink,
           (long long)st.st_atim.tv_sec,
           (long long)st.st_mtim.tv_sec,
           (long long)st.st_ctim.tv_sec,
           (long long)0 /* birth */);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Format-driven output                                                 */
/* ------------------------------------------------------------------ */

static int print_formatted(const char *path, const char *fmt,
                             int opt_fs, int follow_links, int use_utc,
                             int add_newline)
{
    strbuf_t sb;
    if (sb_init(&sb, 256) < 0) {
        err_msg("stat", "out of memory");
        return 1;
    }

    struct stat st;
    struct statvfs sv;

    if (opt_fs) {
        if (statvfs(path, &sv) != 0) {
            err_sys("stat", "cannot statfs '%s'", path);
            sb_free(&sb);
            return 1;
        }
    } else {
        int rc = follow_links ? stat(path, &st) : lstat(path, &st);
        if (rc != 0) {
            err_sys("stat", "cannot stat '%s'", path);
            sb_free(&sb);
            return 1;
        }
    }

    /* Walk the format string */
    for (const char *p = fmt; *p; ) {
        if (*p == '%') {
            p++;
            if (*p == '\0') break;
            if (opt_fs) {
                if (process_fs_directive(&p, path, &sv, &sb) != 0) {
                    err_msg("stat", "out of memory");
                    sb_free(&sb);
                    return 1;
                }
            } else {
                if (process_directive(&p, path, &st, &sb, use_utc) != 0) {
                    err_msg("stat", "out of memory");
                    sb_free(&sb);
                    return 1;
                }
            }
        } else if (*p == '\\') {
            /* Expand single escape via expand_escapes helper */
            char two[3] = { p[0], p[1], '\0' };
            strbuf_t tmp_sb;
            if (sb_init(&tmp_sb, 4) < 0) {
                err_msg("stat", "out of memory");
                sb_free(&sb);
                return 1;
            }
            expand_escapes(two, &tmp_sb);
            if (sb_appendn(&sb, sb_str(&tmp_sb), sb_len(&tmp_sb)) < 0) {
                sb_free(&tmp_sb);
                err_msg("stat", "out of memory");
                sb_free(&sb);
                return 1;
            }
            sb_free(&tmp_sb);
            p += (p[1] ? 2 : 1);
        } else {
            if (sb_appendc(&sb, *p) < 0) {
                err_msg("stat", "out of memory");
                sb_free(&sb);
                return 1;
            }
            p++;
        }
    }

    fwrite(sb_str(&sb), 1, sb_len(&sb), stdout);
    if (add_newline)
        putchar('\n');

    sb_free(&sb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int applet_stat(int argc, char **argv)
{
    int opt_fs      = 0;   /* -f: filesystem stats */
    int opt_terse   = 0;   /* -t: terse */
    int opt_nofollow = 0;  /* -L would mean follow; by default no follow */
    int opt_follow  = 1;   /* by default, follow links (like GNU stat) */
    int use_utc     = 0;
    int add_newline = 1;   /* --printf suppresses trailing newline */
    const char *format  = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        if (strncmp(arg, "--format=", 9) == 0) {
            format = arg + 9;
            add_newline = 1;
            continue;
        }
        if (strncmp(arg, "--printf=", 9) == 0) {
            format = arg + 9;
            add_newline = 0;
            continue;
        }
        if (strcmp(arg, "--terse") == 0)         { opt_terse = 1; continue; }
        if (strcmp(arg, "--dereference") == 0)   { opt_follow = 1; opt_nofollow = 0; continue; }
        if (strcmp(arg, "--no-dereference") == 0){ opt_nofollow = 1; opt_follow = 0; continue; }
        if (strcmp(arg, "--file-system") == 0)   { opt_fs = 1; continue; }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'f': opt_fs = 1; break;
            case 't': opt_terse = 1; break;
            case 'L': opt_follow = 1; opt_nofollow = 0; break;
            case 'c':
                /* -c FORMAT */
                if (p[1]) {
                    format = p + 1;
                    stop = 1;
                } else {
                    i++;
                    if (i >= argc) {
                        err_usage("stat", "[-ftL] [-c FORMAT] FILE...");
                        return 1;
                    }
                    format = argv[i];
                }
                add_newline = 1;
                break;
            default:
                err_msg("stat", "unrecognized option '-%c'", *p);
                err_usage("stat", "[-ftL] [-c FORMAT] FILE...");
                return 1;
            }
            p++;
        }
    }

    if (i >= argc) {
        err_usage("stat", "[-ftL] [-c FORMAT] FILE...");
        return 1;
    }

    int follow_links = opt_follow && !opt_nofollow;
    int ret = 0;

    for (; i < argc; i++) {
        const char *path = argv[i];

        if (format) {
            if (print_formatted(path, format, opt_fs, follow_links,
                                 use_utc, add_newline) != 0)
                ret = 1;
        } else if (opt_terse) {
            if (print_terse(path, opt_fs, follow_links, use_utc) != 0)
                ret = 1;
        } else {
            if (print_default(path, opt_fs, follow_links, use_utc) != 0)
                ret = 1;
        }
    }

    return ret;
}
