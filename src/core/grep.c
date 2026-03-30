/* grep.c — grep builtin: search files for patterns */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/path.h"
#include "../util/strbuf.h"
#include "../util/charclass.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Maximum number of -e patterns or -f pattern files */
#define MAX_PATTERNS 256
/* Maximum number of --include/--exclude globs each */
#define MAX_GLOBS    64

/* ------------------------------------------------------------------ */
/* Option state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int opt_E;           /* -E: extended (ERE) */
    int opt_F;           /* -F: fixed string */
    int opt_c;           /* -c: count matches */
    int opt_i;           /* -i: case insensitive */
    int opt_l;           /* -l: list filenames only */
    int opt_n;           /* -n: print line numbers */
    int opt_q;           /* -q: quiet */
    int opt_s;           /* -s: suppress file errors */
    int opt_v;           /* -v: invert match */
    int opt_w;           /* -w: word boundaries */
    int opt_r;           /* -r/-R: recursive */
    int opt_color;       /* --color: ANSI highlight */

    /* Patterns */
    char  *patterns[MAX_PATTERNS];
    int    npatterns;

    /* Compiled regexes (one per pattern, when not -F) */
    regex_t *compiled[MAX_PATTERNS];
    int      compiled_ok[MAX_PATTERNS]; /* 1 if compiled[i] is valid */

    /* Glob filters */
    char *include_globs[MAX_GLOBS];
    int   n_include;
    char *exclude_globs[MAX_GLOBS];
    int   n_exclude;
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
 * Respects -w (word boundaries checked manually for BRE/fixed).
 * Returns 1 if matched.
 */
static int regex_match(const regex_t *re, const char *line, int opt_w)
{
    regmatch_t m;
    int start = 0;
    int len   = (int)strlen(line);

    for (;;) {
        if (regexec(re, line + start, 1, &m, start > 0 ? REG_NOTBOL : 0) != 0)
            return 0;
        if (!opt_w)
            return 1;
        int ms = start + (int)m.rm_so;
        int me = start + (int)m.rm_eo;
        int before_ok = (ms == 0) || !is_word_char(line[ms - 1]);
        int after_ok  = (me >= len) || !is_word_char(line[me]);
        if (before_ok && after_ok)
            return 1;
        /* Advance past this match and try again */
        start += (int)m.rm_eo;
        if (start >= len)
            return 0;
    }
}

/*
 * Build a word-boundary-wrapped ERE pattern from raw pattern.
 * Result is allocated with malloc; caller frees.
 */
static char *wrap_word_bre(const char *pat)
{
    /* Use \b...\b in ERE; works with POSIX extended */
    size_t plen = strlen(pat);
    size_t total = plen + 9; /* \b + pat + \b + NUL */
    char *out = malloc(total);
    if (!out)
        return NULL;
    int r = snprintf(out, total, "\\b%s\\b", pat);
    if (r < 0 || (size_t)r >= total) {
        free(out);
        return NULL;
    }
    return out;
}

/* Compile all patterns into compiled[]. */
static int compile_patterns(grep_opts_t *g)
{
    int flags = REG_NOSUB;
    if (g->opt_E)
        flags |= REG_EXTENDED;
    if (g->opt_i)
        flags |= REG_ICASE;

    for (int i = 0; i < g->npatterns; i++) {
        g->compiled[i] = malloc(sizeof(regex_t));
        if (!g->compiled[i]) {
            err_msg("grep", "out of memory");
            return 1;
        }
        const char *pat = g->patterns[i];
        char *word_pat  = NULL;

        /* -w with ERE: wrap in \b...\b and compile as ERE */
        if (g->opt_w && g->opt_E) {
            word_pat = wrap_word_bre(pat);
            if (!word_pat) {
                err_msg("grep", "out of memory");
                free(g->compiled[i]);
                g->compiled[i] = NULL;
                return 1;
            }
            pat = word_pat;
        }

        int rc = regcomp(g->compiled[i], pat, flags);
        free(word_pat);
        if (rc != 0) {
            char errbuf[256];
            regerror(rc, g->compiled[i], errbuf, sizeof(errbuf));
            err_msg("grep", "invalid regex '%s': %s", g->patterns[i], errbuf);
            free(g->compiled[i]);
            g->compiled[i] = NULL;
            return 1;
        }
        g->compiled_ok[i] = 1;
    }
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
            m = regex_match(g->compiled[i], line, g->opt_w && !g->opt_E);
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
 * Uses REG_NOTBOL for matches after the first.
 */
static void print_line_color_regex(const regex_t *re, const char *line)
{
    const char *p   = line;
    int         off = 0;
    regmatch_t  m;

    for (;;) {
        int eflags = (off > 0) ? REG_NOTBOL : 0;
        if (regexec(re, p, 1, &m, eflags) != 0 || m.rm_so == m.rm_eo) {
            fputs(p, stdout);
            break;
        }
        fwrite(p, 1, (size_t)m.rm_so, stdout);
        fputs(COL_MATCH, stdout);
        fwrite(p + m.rm_so, 1, (size_t)(m.rm_eo - m.rm_so), stdout);
        fputs(COL_RESET, stdout);
        p   += m.rm_eo;
        off += (int)m.rm_eo;
        if (*p == '\0')
            break;
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

    /* Use a strbuf to build output lines */
    strbuf_t out;
    if (sb_init(&out, 256) != 0) {
        err_msg("grep", "out of memory");
        return 2;
    }

    while ((linelen = getline(&linebuf, &linecap, fp)) >= 0) {
        linenum++;

        /* Binary detection: check for NUL bytes */
        if (!binary) {
            for (ssize_t k = 0; k < linelen; k++) {
                if (linebuf[k] == '\0') {
                    binary = 1;
                    break;
                }
            }
        }

        if (binary) {
            /* Binary file: stop line scanning */
            break;
        }

        /* Strip trailing newline for matching */
        if (linelen > 0 && linebuf[linelen - 1] == '\n') {
            linebuf[linelen - 1] = '\0';
            linelen--;
        }

        int matched = line_matches_any(linebuf, g);
        if (g->opt_v)
            matched = !matched;

        if (!matched)
            continue;

        count++;
        result = 0;
        g_matched_any = 1;

        if (g->opt_q)
            goto done;
        if (g->opt_l) {
            printf("%s\n", filename ? filename : "(stdin)");
            goto done;
        }
        if (g->opt_c)
            continue; /* accumulate count, print at end */

        /* Build output line */
        sb_reset(&out);

        if (show_fname && filename) {
            if (g->opt_color) {
                if (sb_append(&out, COL_FNAME)  != 0) goto oom;
                if (sb_append(&out, filename)    != 0) goto oom;
                if (sb_append(&out, COL_RESET)   != 0) goto oom;
                if (sb_append(&out, COL_SEP ":" COL_RESET) != 0) goto oom;
            } else {
                if (sb_append(&out, filename) != 0) goto oom;
                if (sb_appendc(&out, ':')     != 0) goto oom;
            }
        }

        if (g->opt_n) {
            if (g->opt_color) {
                if (sb_appendf(&out, COL_LINENO "%ld" COL_RESET
                               COL_SEP ":" COL_RESET, linenum) != 0) goto oom;
            } else {
                if (sb_appendf(&out, "%ld:", linenum) != 0) goto oom;
            }
        }

        /* Print prefix, then the line (with optional color) */
        fputs(sb_str(&out), stdout);

        if (g->opt_color && !g->opt_F && g->npatterns > 0 && g->compiled_ok[0]) {
            /* Color-highlight first compiled pattern */
            print_line_color_regex(g->compiled[0], linebuf);
        } else if (g->opt_color && g->opt_F && g->npatterns > 0) {
            print_line_color_fixed(linebuf, g->patterns[0], g->opt_i);
        } else {
            fputs(linebuf, stdout);
        }
        putchar('\n');
    }

    if (ferror(fp)) {
        if (!g->opt_s && filename)
            err_sys("grep", "%s", filename);
        sb_free(&out);
        free(linebuf);
        return 2;
    }

    if (binary) {
        if (!g->opt_q && !g->opt_l) {
            /* Drain rest of file to determine if any match — not practical,
             * just report binary file matches if we got here via content scan */
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
    sb_free(&out);
    free(linebuf);
    return result;

oom:
    err_msg("grep", "out of memory building output line");
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

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (!g->opt_s)
            err_sys("grep", "%s", path);
        return 2;
    }
    posix_fadvise(fileno(fp), 0, 0, POSIX_FADV_SEQUENTIAL); /* advisory */

    int r = grep_stream(fp, path, show_fname, g);
    fclose(fp);
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
        if (g->compiled_ok[i]) {
            regfree(g->compiled[i]);
        }
        free(g->compiled[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Main applet entry                                                   */
/* ------------------------------------------------------------------ */

int applet_grep(int argc, char **argv)
{
    grep_opts_t g;
    memset(&g, 0, sizeof(g));

    /* --color: only if stdout is a tty */
    int color_auto = isatty(STDOUT_FILENO);

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
            case 'n': g.opt_n = 1; break;
            case 'q': g.opt_q = 1; break;
            case 's': g.opt_s = 1; break;
            case 'v': g.opt_v = 1; break;
            case 'w': g.opt_w = 1; break;
            case 'r': case 'R': g.opt_r = 1; break;
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
        int show_fname = (nfiles > 1) || g.opt_r;
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
