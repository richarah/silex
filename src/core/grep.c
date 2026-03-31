/* grep.c — grep builtin: search files for patterns */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"
#include "../util/strbuf.h"
#include "../util/charclass.h"
#include "../util/regex/regex.h"
#include "../util/vcsignore.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Maximum number of -e patterns or -f pattern files */
#define MAX_PATTERNS 256
/* Maximum number of --include/--exclude globs each */
#define MAX_GLOBS    64
/* Large stdio read buffer — reduces getline() syscall count from ~136 to ~6
 * for a 550KB file (system grep uses 96KB mmap-style reads). */
#define GREP_STDIO_BUFSZ (1 << 17)  /* 128 KB */

/* ------------------------------------------------------------------ */
/* Option state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int opt_E;           /* -E: extended (ERE) */
    int opt_F;           /* -F: fixed string */
    int opt_c;           /* -c: count matches */
    int opt_i;           /* -i: case insensitive */
    int opt_l;           /* -l: list filenames only */
    int opt_L;           /* -L: list files with NO match */
    int opt_H;           /* -H: always print filename prefix */
    int opt_n;           /* -n: print line numbers */
    int opt_q;           /* -q: quiet */
    int opt_s;           /* -s: suppress file errors */
    int opt_v;           /* -v: invert match */
    int opt_w;           /* -w: word boundaries */
    int opt_r;           /* -r/-R: recursive */
    int opt_color;       /* --color: ANSI highlight */
    int opt_o;           /* -o: only matching portion */
    int max_matches;     /* -m N: stop after N matches (0 = unlimited) */
    int context_A;       /* -A N: N lines of after-context */
    int context_B;       /* -B N: N lines of before-context */

    /* Patterns */
    char  *patterns[MAX_PATTERNS];
    int    npatterns;

    /* Compiled regexes (one per pattern, when not -F) */
    mb_regex *mb_compiled[MAX_PATTERNS];
    int       compiled_ok[MAX_PATTERNS]; /* 1 if mb_compiled[i] is valid */

    /* Glob filters */
    char *include_globs[MAX_GLOBS];
    int   n_include;
    char *exclude_globs[MAX_GLOBS];
    int   n_exclude;

    /* Prefilter: required first literal byte (0 = disabled) */
    unsigned char prefilter_char;

    /* --vcs: skip VCS dirs, hidden files, binary files */
    int opt_vcs;
    /* -S: smart case (case-insensitive when pattern is all-lowercase) */
    int opt_smart;
    /* VCS ignore context (loaded when opt_vcs) */
    vcsignore_t *vcs_ign;
} grep_opts_t;

/* ------------------------------------------------------------------ */
/* Global state shared across recursive traversal                     */
/* ------------------------------------------------------------------ */

static int g_matched_any = 0; /* set to 1 if any match ever found */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Case-fold search: locate needle in haystack (case insensitive).
 * Returns pointer to first match, or NULL.
 *
 * Precomputes the lowercase first character of needle so the outer loop
 * can reject non-matching positions with a single comparison.
 */
static const char *stristr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return haystack;
    int fc = tolower((unsigned char)*needle);  /* first char, precomputed */
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == fc) {
            size_t i;
            for (i = 1; i < nlen; i++) {
                if (tolower((unsigned char)haystack[i]) !=
                    tolower((unsigned char)needle[i]))
                    break;
            }
            if (i == nlen)
                return haystack;
        }
    }
    return NULL;
}

/* Check whether character is a word character for -w */
static int is_word_char(char c)
{
    return is_name_char((unsigned char)c);
}

/*
 * Fixed-string match for one pattern against line.
 * Respects -i (case fold) and -w (word boundary).
 * Returns 1 if matched.
 */
static int fixed_match(const char *line, const char *pattern,
                       int opt_i, int opt_w)
{
    size_t plen = strlen(pattern);
    if (plen == 0)
        return 1;

    const char *p = line;
    for (;;) {
        const char *found = opt_i ? stristr(p, pattern) : strstr(p, pattern);
        if (!found)
            return 0;
        if (opt_w) {
            /* Check boundaries */
            int before_ok = (found == line) || !is_word_char(*(found - 1));
            int after_ok  = !is_word_char(*(found + plen));
            if (!before_ok || !after_ok) {
                p = found + 1;
                continue;
            }
        }
        return 1;
    }
}

/*
 * Regex match for one compiled pattern against line.
 * Respects -w (word boundaries checked manually).
 * Returns 1 if matched.
 */
static int regex_match(const mb_regex *re, const char *line, int opt_w)
{
    size_t len = strlen(line);

    if (!opt_w) {
        return mb_regex_search(re, line, len, NULL);
    }

    /* -w: check word boundaries */
    const char *p   = line;
    size_t      rem = len;

    for (;;) {
        mb_match m;
        if (!mb_regex_search(re, p, rem, &m))
            return 0;
        int ms = (int)(m.start - line);
        int me = (int)(m.end   - line);
        int before_ok = (ms == 0) || !is_word_char(line[ms - 1]);
        int after_ok  = (me >= (int)len) || !is_word_char(line[me]);
        if (before_ok && after_ok)
            return 1;
        /* Advance past this match */
        size_t advance = (size_t)(m.end - p);
        if (advance == 0) advance = 1;
        if (advance > rem) return 0;
        p   += advance;
        rem -= advance;
    }
}


/* Compile all patterns into mb_compiled[]. */
static int compile_patterns(grep_opts_t *g)
{
    /* Smart case (-S): if all patterns are all-lowercase, enable -i.
     * Explicit -i always wins. */
    if (g->opt_smart && !g->opt_i) {
        int all_lower = 1;
        for (int j = 0; j < g->npatterns && all_lower; j++) {
            for (const char *c = g->patterns[j]; *c; c++) {
                if (isupper((unsigned char)*c)) { all_lower = 0; break; }
            }
        }
        if (all_lower) g->opt_i = 1;
    }

    int mb_flags = SX_REG_NOSUB;
    if (g->opt_E)
        mb_flags |= SX_REG_ERE;
    if (g->opt_i)
        mb_flags |= SX_REG_ICASE;

    for (int i = 0; i < g->npatterns; i++) {
        const char *err = NULL;
        g->mb_compiled[i] = mb_regex_compile(g->patterns[i], mb_flags, &err);
        if (!g->mb_compiled[i]) {
            err_msg("grep", "invalid regex '%s': %s", g->patterns[i],
                    err ? err : "unknown error");
            return 1;
        }
        g->compiled_ok[i] = 1;
    }

    /* Prefilter: single pattern, not inverted, not case-insensitive */
    if (g->npatterns == 1 && !g->opt_v && !g->opt_i && g->compiled_ok[0])
        g->prefilter_char = mb_regex_first_char(g->mb_compiled[0]);

    return 0;
}

/*
 * Test line against all patterns.
 * Returns 1 if ANY pattern matches (before -v inversion).
 */
static int line_matches_any(const char *line, const grep_opts_t *g)
{
    for (int i = 0; i < g->npatterns; i++) {
        int m;
        if (g->opt_F) {
            m = fixed_match(line, g->patterns[i], g->opt_i, g->opt_w);
        } else {
            if (!g->compiled_ok[i])
                continue;
            m = regex_match(g->mb_compiled[i], line, g->opt_w);
        }
        if (m)
            return 1;
    }
    return 0;
}

/* ANSI color codes */
#define COL_MATCH   "\033[01;31m"
#define COL_FNAME   "\033[35m"
#define COL_LINENO  "\033[32m"
#define COL_SEP     "\033[36m"
#define COL_RESET   "\033[0m"

/*
 * Highlight a single fixed-string match in line with ANSI codes.
 * Prints the line (with prefix already printed).
 */
static void print_line_color_fixed(const char *line, const char *pattern,
                                   int opt_i)
{
    size_t plen = strlen(pattern);
    const char *p = line;
    for (;;) {
        const char *found = opt_i ? stristr(p, pattern) : strstr(p, pattern);
        if (!found) {
            fputs(p, stdout);
            break;
        }
        /* Print up to match */
        fwrite(p, 1, (size_t)(found - p), stdout);
        fputs(COL_MATCH, stdout);
        fwrite(found, 1, plen, stdout);
        fputs(COL_RESET, stdout);
        p = found + plen;
    }
}

/*
 * Highlight regex matches in line with ANSI codes.
 */
static void print_line_color_regex(const mb_regex *re, const char *line)
{
    const char *p   = line;
    size_t      rem = strlen(line);

    for (;;) {
        mb_match m;
        if (!mb_regex_search(re, p, rem, &m) || m.start == m.end) {
            fwrite(p, 1, rem, stdout);
            break;
        }
        fwrite(p, 1, (size_t)(m.start - p), stdout);
        fputs(COL_MATCH, stdout);
        fwrite(m.start, 1, (size_t)(m.end - m.start), stdout);
        fputs(COL_RESET, stdout);
        size_t advance = (size_t)(m.end - p);
        if (advance == 0) advance = 1;
        if (advance > rem) break;
        p   += advance;
        rem -= advance;
    }
    if (*p != '\0') fputs(p, stdout);
}

/* Print the line prefix (filename + linenum) into strbuf out */
static int grep_print_prefix(strbuf_t *out, const char *filename,
                              int show_fname, long linenum,
                              const grep_opts_t *g, int is_context)
{
    if (show_fname && filename) {
        if (g->opt_color) {
            if (sb_append(out, COL_FNAME)  != 0) return -1;
            if (sb_append(out, filename)    != 0) return -1;
            if (sb_append(out, COL_RESET)   != 0) return -1;
            if (sb_appendc(out, is_context ? '-' : ':') != 0) return -1;
        } else {
            if (sb_append(out, filename) != 0) return -1;
            if (sb_appendc(out, is_context ? '-' : ':') != 0) return -1;
        }
    }
    if (g->opt_n) {
        if (g->opt_color) {
            if (sb_appendf(out, COL_LINENO "%ld" COL_RESET "%c",
                           linenum, is_context ? '-' : ':') != 0) return -1;
        } else {
            if (sb_appendf(out, "%ld%c", linenum,
                           is_context ? '-' : ':') != 0) return -1;
        }
    }
    return 0;
}

/* Print matched portions of line (for -o flag) */
static void grep_print_only_match(const char *line, const grep_opts_t *g,
                                  const char *prefix)
{
    if (g->opt_F) {
        if (g->npatterns == 0) return;
        size_t plen = strlen(g->patterns[0]);
        const char *p = line;
        for (;;) {
            const char *found = g->opt_i ? stristr(p, g->patterns[0])
                                          : strstr(p, g->patterns[0]);
            if (!found) break;
            printf("%s", prefix);
            fwrite(found, 1, plen, stdout);
            putchar('\n');
            p = found + (plen > 0 ? plen : 1);
        }
    } else {
        if (g->npatterns == 0 || !g->compiled_ok[0]) return;
        const char *p   = line;
        size_t      rem = strlen(line);
        for (;;) {
            mb_match m;
            if (!mb_regex_search(g->mb_compiled[0], p, rem, &m)) break;
            size_t mlen = (size_t)(m.end - m.start);
            printf("%s", prefix);
            fwrite(m.start, 1, mlen, stdout);
            putchar('\n');
            size_t advance = (size_t)(m.end - p);
            if (advance == 0) advance = 1;
            if (advance > rem) break;
            p   += advance;
            rem -= advance;
        }
    }
}

/*
 * Search one open FILE stream for matches.
 * filename: displayed name (NULL if reading from stdin with no label).
 * show_fname: prepend filename to output lines.
 * Returns 0 if matches found, 1 if none, 2 on read error.
 */
static int grep_stream(FILE *fp, const char *filename,
                       int show_fname, const grep_opts_t *g)
{
    char    *linebuf = NULL;
    size_t   linecap = 0;
    ssize_t  linelen;
    long     linenum  = 0;
    long     count    = 0;
    int      binary   = 0;
    int      result   = 1; /* assume no match */

    /* Context support */
    int ctx_B = g->context_B;
    int ctx_A = g->context_A;
    int after_left  = 0;
    long last_printed = -1;

    char  **before_buf  = NULL;
    long   *before_lnum = NULL;
    int     before_pos  = 0;
    int     before_full = 0;
    if (ctx_B > 0) {
        before_buf  = calloc((size_t)ctx_B, sizeof(char *));
        before_lnum = calloc((size_t)ctx_B, sizeof(long));
        if (!before_buf || !before_lnum) {
            free(before_buf); free(before_lnum);
            err_msg("grep", "out of memory");
            return 2;
        }
    }

    /* Use a strbuf to build output lines */
    strbuf_t out;
    if (sb_init(&out, 256) != 0) {
        free(before_buf); free(before_lnum);
        err_msg("grep", "out of memory");
        return 2;
    }

    while ((linelen = getline(&linebuf, &linecap, fp)) >= 0) {
        linenum++;

        /* Binary detection: check for NUL bytes */
        if (!binary) {
            for (ssize_t k = 0; k < linelen; k++) {
                if (linebuf[k] == '\0') { binary = 1; break; }
            }
        }
        if (binary) break;

        /* Strip trailing newline for matching */
        if (linelen > 0 && linebuf[linelen - 1] == '\n') {
            linebuf[linelen - 1] = '\0';
            linelen--;
        }

        /* Prefilter: if required first char is absent, line cannot match */
        if (g->prefilter_char &&
            !memchr(linebuf, g->prefilter_char, (size_t)linelen)) {
            if (ctx_B > 0) {
                free(before_buf[before_pos]);
                before_buf[before_pos]  = strdup(linebuf);
                before_lnum[before_pos] = linenum;
                before_pos = (before_pos + 1) % ctx_B;
                if (!before_full && before_pos == 0) before_full = 1;
            }
            continue;
        }

        int matched = line_matches_any(linebuf, g);
        if (g->opt_v) matched = !matched;

        if (!matched) {
            /* Print as after-context if needed */
            if (after_left > 0 && !g->opt_c && !g->opt_q && !g->opt_l && !g->opt_L) {
                sb_reset(&out);
                if (grep_print_prefix(&out, filename, show_fname,
                                       linenum, g, 1) < 0) goto oom;
                fputs(sb_str(&out), stdout);
                fputs(linebuf, stdout);
                putchar('\n');
                last_printed = linenum;
                after_left--;
            }
            /* Store in before-context ring buffer */
            if (ctx_B > 0) {
                free(before_buf[before_pos]);
                before_buf[before_pos]  = strdup(linebuf);
                before_lnum[before_pos] = linenum;
                before_pos = (before_pos + 1) % ctx_B;
                if (!before_full && before_pos == 0) before_full = 1;
            }
            continue;
        }

        count++;
        result = 0;
        g_matched_any = 1;

        /* -m: stop after max_matches */
        if (g->max_matches > 0 && count > g->max_matches) {
            count--;
            result = (count > 0) ? 0 : 1;
            goto done;
        }

        if (g->opt_q) goto done;
        if (g->opt_L) continue; /* suppress output; track match status only */
        if (g->opt_l) {
            printf("%s\n", filename ? filename : "(stdin)");
            goto done;
        }
        if (g->opt_c) {
            if (ctx_B > 0) {
                free(before_buf[before_pos]);
                before_buf[before_pos]  = strdup(linebuf);
                before_lnum[before_pos] = linenum;
                before_pos = (before_pos + 1) % ctx_B;
                if (!before_full && before_pos == 0) before_full = 1;
            }
            continue;
        }

        /* Print separator between context groups */
        if ((ctx_A > 0 || ctx_B > 0) && last_printed >= 0 &&
            last_printed < linenum - 1) {
            puts("--");
        }

        /* Print before-context lines */
        if (ctx_B > 0) {
            int n_avail = before_full ? ctx_B : before_pos;
            int start   = before_full ? before_pos : 0;
            for (int k = 0; k < n_avail; k++) {
                int idx = (start + k) % ctx_B;
                if (!before_buf[idx] || before_lnum[idx] <= last_printed) continue;
                sb_reset(&out);
                if (grep_print_prefix(&out, filename, show_fname,
                                       before_lnum[idx], g, 1) < 0) goto oom;
                fputs(sb_str(&out), stdout);
                fputs(before_buf[idx], stdout);
                putchar('\n');
                last_printed = before_lnum[idx];
            }
            for (int k = 0; k < ctx_B; k++) {
                free(before_buf[k]); before_buf[k] = NULL; before_lnum[k] = 0;
            }
            before_pos = 0; before_full = 0;
        }

        /* Build prefix for this matched line */
        sb_reset(&out);
        if (grep_print_prefix(&out, filename, show_fname, linenum, g, 0) < 0)
            goto oom;
        const char *prefix_str = sb_str(&out);

        if (g->opt_o) {
            grep_print_only_match(linebuf, g, prefix_str);
        } else {
            fputs(prefix_str, stdout);
            if (g->opt_color && !g->opt_F && g->npatterns > 0 && g->compiled_ok[0]) {
                print_line_color_regex(g->mb_compiled[0], linebuf);
            } else if (g->opt_color && g->opt_F && g->npatterns > 0) {
                print_line_color_fixed(linebuf, g->patterns[0], g->opt_i);
            } else {
                fputs(linebuf, stdout);
            }
            putchar('\n');
        }
        last_printed = linenum;
        after_left   = ctx_A;

        if (ctx_B > 0) {
            free(before_buf[before_pos]);
            before_buf[before_pos]  = strdup(linebuf);
            before_lnum[before_pos] = linenum;
            before_pos = (before_pos + 1) % ctx_B;
            if (!before_full && before_pos == 0) before_full = 1;
        }
    }

    if (ferror(fp)) {
        if (!g->opt_s && filename)
            err_sys("grep", "%s", filename);
        goto cleanup_err;
    }

    if (binary) {
        if (!g->opt_q && !g->opt_l) {
            printf("Binary file %s matches\n",
                   filename ? filename : "(stdin)");
        } else if (g->opt_l && filename) {
            printf("%s\n", filename);
        }
        result = 0;
        g_matched_any = 1;
    }

    if (g->opt_c) {
        if (show_fname && filename)
            printf("%s:%ld\n", filename, count);
        else
            printf("%ld\n", count);
        if (count > 0)
            result = 0;
    }

done:
    if (ctx_B > 0) {
        for (int k = 0; k < ctx_B; k++) free(before_buf[k]);
        free(before_buf); free(before_lnum);
    }
    sb_free(&out);
    free(linebuf);
    return result;

oom:
    err_msg("grep", "out of memory building output line");
cleanup_err:
    if (ctx_B > 0) {
        for (int k = 0; k < ctx_B; k++) free(before_buf[k]);
        free(before_buf); free(before_lnum);
    }
    sb_free(&out);
    free(linebuf);
    return 2;
}

/* Check if filename matches any exclude glob or fails include globs. */
static int glob_excluded(const char *basename, const grep_opts_t *g)
{
    for (int i = 0; i < g->n_exclude; i++) {
        if (fnmatch(g->exclude_globs[i], basename, 0) == 0)
            return 1;
    }
    if (g->n_include > 0) {
        int ok = 0;
        for (int i = 0; i < g->n_include; i++) {
            if (fnmatch(g->include_globs[i], basename, 0) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return 1;
    }
    return 0;
}

/* Forward declaration for recursive use */
static int grep_path(const char *path, int show_fname, const grep_opts_t *g);

/* Recursively grep a directory. */
static int grep_dir(const char *dirpath, const grep_opts_t *g)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (!g->opt_s)
            err_sys("grep", "%s", dirpath);
        return 2;
    }

    int result = 1;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* --vcs: skip hidden entries and VCS/noise dirs */
        if (g->opt_vcs) {
            if (ent->d_name[0] == '.') continue; /* hidden */
            int is_dir = 0;
#ifdef DT_DIR
            if (ent->d_type == DT_DIR)      is_dir = 1;
            else if (ent->d_type == DT_UNKNOWN) {
                struct stat st2;
                char tmp[PATH_MAX];
                if (path_join(dirpath, ent->d_name, tmp) && lstat(tmp, &st2) == 0)
                    is_dir = S_ISDIR(st2.st_mode);
            }
#endif
            if (vcsignore_skip_name(ent->d_name, is_dir)) continue;
            /* Check .gitignore rules against basename (handles *.ext, dir/) */
            if (g->vcs_ign && vcsignore_match(g->vcs_ign, ent->d_name, is_dir))
                continue;
        }

        char child[PATH_MAX];
        if (!path_join(dirpath, ent->d_name, child)) {
            err_msg("grep", "path too long: %s/%s", dirpath, ent->d_name);
            continue;
        }

        int r = grep_path(child, 1, g);
        if (r == 0)
            result = 0;
    }

    closedir(dir);
    return result;
}

/* Grep one path (file or directory). show_fname: prepend filename. */
static int grep_path(const char *path, int show_fname, const grep_opts_t *g)
{
    /* POSIX: "-" means stdin */
    if (strcmp(path, "-") == 0) {
        return grep_stream(stdin, "(standard input)", show_fname, g);
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        if (!g->opt_s)
            err_sys("grep", "%s", path);
        return 2;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!g->opt_r) {
            if (!g->opt_s)
                err_msg("grep", "%s: is a directory", path);
            return 2;
        }
        return grep_dir(path, g);
    }

    if (!S_ISREG(st.st_mode))
        return 1; /* skip special files */

    /* Apply glob filters on basename */
    if (glob_excluded(path_basename(path), g))
        return 1;

    /* --vcs: skip hidden files and noise extensions */
    if (g->opt_vcs) {
        const char *base = path_basename(path);
        if (base[0] == '.') return 1;  /* hidden */
        if (vcsignore_skip_name(base, 0)) return 1;
        /* Check .gitignore rules against basename */
        if (g->vcs_ign && vcsignore_match(g->vcs_ign, base, 0)) return 1;
    }

    /* --vcs: silently skip binary files (null-byte check in first 512 bytes).
     * Use raw read(2) before fopen to avoid stdio buffering interactions. */
    if (g->opt_vcs) {
        int bfd = open(path, O_RDONLY);
        if (bfd >= 0) {
            char probe[512];
            ssize_t nr = read(bfd, probe, sizeof(probe));
            close(bfd);
            if (nr > 0 && memchr(probe, '\0', (size_t)nr))
                return 1; /* binary: skip silently */
        }
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (!g->opt_s)
            err_sys("grep", "%s", path);
        return 2;
    }

    posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL); /* advisory */
    /* Enlarge stdio read buffer to reduce getline() syscall overhead.
     * Must supply an explicit buffer — passing NULL to setvbuf() still uses
     * glibc's internal 4KB allocation.  With a 128KB explicit buffer the
     * read() count drops from ~136 to ~5 for a 550KB file. */
    static char grep_iobuf[GREP_STDIO_BUFSZ];
    setvbuf(fp, grep_iobuf, _IOFBF, sizeof(grep_iobuf));

    int r = grep_stream(fp, path, show_fname, g);
    fclose(fp);
    /* -L: print filename of files with NO match */
    if (g->opt_L && r == 1) {
        printf("%s\n", path);
        return 1; /* still "no match" for exit status */
    }
    return r;
}

/* Load patterns from a file (-f FILE). */
static int load_pattern_file(const char *fname, grep_opts_t *g)
{
    FILE *fp = fopen(fname, "r");
    if (!fp) {
        err_sys("grep", "%s", fname);
        return 1;
    }

    char   *line    = NULL;
    size_t  cap     = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, fp)) >= 0) {
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (g->npatterns >= MAX_PATTERNS) {
            err_msg("grep", "too many patterns (max %d)", MAX_PATTERNS);
            free(line);
            fclose(fp);
            return 1;
        }
        g->patterns[g->npatterns] = strdup(line);
        if (!g->patterns[g->npatterns]) {
            err_msg("grep", "out of memory");
            free(line);
            fclose(fp);
            return 1;
        }
        g->npatterns++;
    }

    free(line);
    fclose(fp);
    return 0;
}

/* Free all resources in grep_opts_t */
static void grep_opts_free(grep_opts_t *g)
{
    for (int i = 0; i < g->npatterns; i++) {
        free(g->patterns[i]);
        if (g->compiled_ok[i] && g->mb_compiled[i])
            mb_regex_free(g->mb_compiled[i]);
    }
    if (g->vcs_ign) { vcsignore_free(g->vcs_ign); g->vcs_ign = NULL; }
}

/* ------------------------------------------------------------------ */
/* Main applet entry                                                   */
/* ------------------------------------------------------------------ */

int applet_grep(int argc, char **argv)
{
    grep_opts_t g;
    memset(&g, 0, sizeof(g));

    /* SILEX_SMART=1: enable smart-case and VCS-aware mode by default */
    if (getenv("SILEX_SMART") && strcmp(getenv("SILEX_SMART"), "1") == 0)
        g.opt_smart = 1;

    /* --color: only if stdout is a tty */
    int color_auto = isatty(STDOUT_FILENO);

    /* Explicit 128KB stdout buffer when not a tty: reduces write() count ~30x
     * (1365 → ~45 for 100k matching lines) without buffering interactive output. */
    if (!color_auto) {
        static char grep_out_buf[131072];
        setvbuf(stdout, grep_out_buf, _IOFBF, sizeof(grep_out_buf));
    }

    int i;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) { i++; break; }

        /* Long options */
        if (strcmp(arg, "--extended-regexp") == 0)   { g.opt_E = 1; continue; }
        if (strcmp(arg, "--fixed-strings")   == 0)   { g.opt_F = 1; continue; }
        if (strcmp(arg, "--count")           == 0)   { g.opt_c = 1; continue; }
        if (strcmp(arg, "--ignore-case")     == 0)   { g.opt_i = 1; continue; }
        if (strcmp(arg, "--files-with-matches") == 0){ g.opt_l = 1; continue; }
        if (strcmp(arg, "--line-number")     == 0)   { g.opt_n = 1; continue; }
        if (strcmp(arg, "--quiet")           == 0 ||
            strcmp(arg, "--silent")          == 0)   { g.opt_q = 1; continue; }
        if (strcmp(arg, "--no-messages")     == 0)   { g.opt_s = 1; continue; }
        if (strcmp(arg, "--invert-match")    == 0)   { g.opt_v = 1; continue; }
        if (strcmp(arg, "--word-regexp")     == 0)   { g.opt_w = 1; continue; }
        if (strcmp(arg, "--recursive")       == 0)   { g.opt_r = 1; continue; }
        if (strcmp(arg, "--vcs")             == 0)   { g.opt_vcs = 1; g.opt_r = 1; continue; }
        if (strcmp(arg, "--smart-case")      == 0)   { g.opt_smart = 1; continue; }
        if (strcmp(arg, "--color")           == 0 ||
            strcmp(arg, "--colour")          == 0 ||
            strcmp(arg, "--color=always")    == 0 ||
            strcmp(arg, "--colour=always")   == 0)   { g.opt_color = 1; continue; }
        if (strcmp(arg, "--color=auto")      == 0 ||
            strcmp(arg, "--colour=auto")     == 0)   { g.opt_color = color_auto; continue; }
        if (strcmp(arg, "--color=never")     == 0 ||
            strcmp(arg, "--colour=never")    == 0)   { g.opt_color = 0; continue; }

        if (strncmp(arg, "--include=", 10) == 0) {
            if (g.n_include >= MAX_GLOBS) { err_msg("grep", "too many --include globs"); return 2; }
            g.include_globs[g.n_include++] = (char *)(arg + 10);
            continue;
        }
        if (strcmp(arg, "--include") == 0) {
            if (++i >= argc) { err_msg("grep", "--include requires argument"); return 2; }
            if (g.n_include >= MAX_GLOBS) { err_msg("grep", "too many --include globs"); return 2; }
            g.include_globs[g.n_include++] = argv[i];
            continue;
        }
        if (strncmp(arg, "--exclude=", 10) == 0) {
            if (g.n_exclude >= MAX_GLOBS) { err_msg("grep", "too many --exclude globs"); return 2; }
            g.exclude_globs[g.n_exclude++] = (char *)(arg + 10);
            continue;
        }
        if (strcmp(arg, "--exclude") == 0) {
            if (++i >= argc) { err_msg("grep", "--exclude requires argument"); return 2; }
            if (g.n_exclude >= MAX_GLOBS) { err_msg("grep", "too many --exclude globs"); return 2; }
            g.exclude_globs[g.n_exclude++] = argv[i];
            continue;
        }

        if (strcmp(arg, "--regexp") == 0 || strcmp(arg, "-e") == 0) {
            /* handled in short-flag section below; but long form: */
            if (strcmp(arg, "--regexp") == 0) {
                if (++i >= argc) { err_msg("grep", "--regexp requires argument"); return 2; }
                if (g.npatterns >= MAX_PATTERNS) { err_msg("grep", "too many patterns"); return 2; }
                g.patterns[g.npatterns] = strdup(argv[i]);
                if (!g.patterns[g.npatterns]) { err_msg("grep", "out of memory"); return 2; }
                g.npatterns++;
                continue;
            }
        }
        if (strcmp(arg, "--file") == 0) {
            if (++i >= argc) { err_msg("grep", "--file requires argument"); return 2; }
            if (load_pattern_file(argv[i], &g) != 0) return 2;
            continue;
        }

        if (arg[0] != '-' || arg[1] == '\0')
            break;

        /* Short options */
        const char *p = arg + 1;
        int stop = 0;
        while (*p && !stop) {
            switch (*p) {
            case 'E': g.opt_E = 1; break;
            case 'F': g.opt_F = 1; break;
            case 'c': g.opt_c = 1; break;
            case 'i': g.opt_i = 1; break;
            case 'l': g.opt_l = 1; break;
            case 'L': g.opt_L = 1; break;
            case 'H': g.opt_H = 1; break;
            case 'n': g.opt_n = 1; break;
            case 'q': g.opt_q = 1; break;
            case 's': g.opt_s = 1; break;
            case 'v': g.opt_v = 1; break;
            case 'w': g.opt_w = 1; break;
            case 'r': case 'R': g.opt_r = 1; break;
            case 'o': g.opt_o = 1; break;
            case 'S': g.opt_smart = 1; break;
            case 'm': {
                const char *val;
                if (p[1]) { val = p + 1; stop = 1; }
                else {
                    if (++i >= argc) {
                        err_msg("grep", "-m requires argument"); return 2;
                    }
                    val = argv[i];
                }
                g.max_matches = atoi(val);
                stop = 1;
                break;
            }
            case 'A': {
                const char *val;
                if (p[1]) { val = p + 1; stop = 1; }
                else {
                    if (++i >= argc) {
                        err_msg("grep", "-A requires argument"); return 2;
                    }
                    val = argv[i];
                }
                g.context_A = atoi(val);
                stop = 1;
                break;
            }
            case 'B': {
                const char *val;
                if (p[1]) { val = p + 1; stop = 1; }
                else {
                    if (++i >= argc) {
                        err_msg("grep", "-B requires argument"); return 2;
                    }
                    val = argv[i];
                }
                g.context_B = atoi(val);
                stop = 1;
                break;
            }
            case 'C': {
                const char *val;
                if (p[1]) { val = p + 1; stop = 1; }
                else {
                    if (++i >= argc) {
                        err_msg("grep", "-C requires argument"); return 2;
                    }
                    val = argv[i];
                }
                g.context_A = g.context_B = atoi(val);
                stop = 1;
                break;
            }
            case 'e': {
                const char *pat;
                if (p[1]) {
                    pat = p + 1;
                    p += strlen(p) - 1;
                } else {
                    if (++i >= argc) {
                        err_msg("grep", "-e requires argument");
                        return 2;
                    }
                    pat = argv[i];
                }
                if (g.npatterns >= MAX_PATTERNS) {
                    err_msg("grep", "too many patterns");
                    return 2;
                }
                g.patterns[g.npatterns] = strdup(pat);
                if (!g.patterns[g.npatterns]) {
                    err_msg("grep", "out of memory");
                    return 2;
                }
                g.npatterns++;
                stop = 1;
                break;
            }
            case 'f': {
                const char *fname;
                if (p[1]) {
                    fname = p + 1;
                    p += strlen(p) - 1;
                } else {
                    if (++i >= argc) {
                        err_msg("grep", "-f requires argument");
                        return 2;
                    }
                    fname = argv[i];
                }
                if (load_pattern_file(fname, &g) != 0)
                    return 2;
                stop = 1;
                break;
            }
            default:
                err_msg("grep", "unrecognized option '-%c'", *p);
                return 2;
            }
            p++;
        }
    }

    /* If no -e/-f patterns, first non-option arg is the pattern */
    if (g.npatterns == 0) {
        if (i >= argc) {
            err_usage("grep", "[-EFciln qsvwrR] [-e PAT] [-f FILE] PATTERN [FILE...]");
            return 2;
        }
        g.patterns[0] = strdup(argv[i]);
        if (!g.patterns[0]) {
            err_msg("grep", "out of memory");
            return 2;
        }
        g.npatterns = 1;
        i++;
    }

    /* SILEX_SMART: when smart mode is active and recursive, enable --vcs */
    if (g.opt_smart && g.opt_r && !g.opt_vcs)
        g.opt_vcs = 1;

    /* Load VCS ignore rules when --vcs.
     * Use the first directory argument as the root, falling back to ".". */
    if (g.opt_vcs) {
        const char *vcs_root = ".";
        for (int k = i; k < argc; k++) {
            struct stat vst;
            if (stat(argv[k], &vst) == 0 && S_ISDIR(vst.st_mode)) {
                vcs_root = argv[k];
                break;
            }
        }
        g.vcs_ign = vcsignore_load(vcs_root);
    }

    /* Compile regex patterns unless -F */
    if (!g.opt_F) {
        if (compile_patterns(&g) != 0) {
            grep_opts_free(&g);
            return 2;
        }
    }

    int final_result = 1; /* no match yet */
    int nfiles = argc - i;

    if (nfiles == 0) {
        /* Read from stdin */
        int r = grep_stream(stdin, NULL, 0, &g);
        if (r == 0)
            final_result = 0;
        else if (r == 2)
            final_result = 2;
    } else {
        int show_fname = (nfiles > 1) || g.opt_r || g.opt_H;
        for (int j = i; j < argc; j++) {
            int r = grep_path(argv[j], show_fname, &g);
            if (r == 0)
                final_result = 0;
            else if (r == 2 && final_result != 0)
                final_result = 2;
        }
    }

    grep_opts_free(&g);

    if (g.opt_q && g_matched_any)
        return 0;
    return final_result;
}
