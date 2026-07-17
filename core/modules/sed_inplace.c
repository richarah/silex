#define _POSIX_C_SOURCE 200809L

/*
 * sed_inplace.c — silex module: sed -i SUFFIX (in-place edit with backup)
 *
 * The core sed builtin handles plain -i (in-place, no backup).  This module
 * extends that behaviour by supporting a backup suffix: sed -iSUFFIX or
 * sed -i SUFFIX will write the original file to <file>SUFFIX before editing.
 *
 * Algorithm:
 *  1. Parse -i<suffix> or -i <suffix> from argv.
 *  2. For each file operand:
 *     a. Read the file into a buffer.
 *     b. Apply sed-style address/substitution commands found in argv.
 *        (For simplicity this module supports only s/pattern/replace/[g] on
 *        each line, which is the common case for in-place editing.)
 *     c. Write original file to <file><suffix> as a backup.
 *     d. Write the transformed content to <file>.
 */

#include "../silex_module.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

/* Maximum line length we handle */
#define MAX_LINE 65536

/* ---- very small s/pat/rep/[g] parser ------------------------------------ */

typedef struct {
    char *pattern;
    char *replace;
    int   global;
} subst_t;

/*
 * Parse a substitution command of the form s/PATTERN/REPLACE/[g].
 * The delimiter may be any non-alphanumeric character after 's'.
 * Returns 1 on success, 0 if the string is not a substitution command.
 * Caller must free s->pattern and s->replace on success.
 */
static int parse_subst(const char *expr, subst_t *s)
{
    if (!expr || expr[0] != 's')
        return 0;

    char delim = expr[1];
    if (!delim || delim == '\0')
        return 0;

    /* Find the three delimiter positions */
    const char *p1 = expr + 2;
    const char *p2 = strchr(p1, delim);
    if (!p2) return 0;

    const char *p3 = strchr(p2 + 1, delim);
    if (!p3) return 0;

    size_t pat_len = (size_t)(p2 - p1);
    size_t rep_len = (size_t)(p3 - (p2 + 1));

    s->pattern = malloc(pat_len + 1);
    s->replace = malloc(rep_len + 1);
    if (!s->pattern || !s->replace) {
        free(s->pattern);
        free(s->replace);
        return 0;
    }

    memcpy(s->pattern, p1, pat_len);
    s->pattern[pat_len] = '\0';

    memcpy(s->replace, p2 + 1, rep_len);
    s->replace[rep_len] = '\0';

    /* Check for 'g' flag */
    s->global = (*(p3 + 1) == 'g') ? 1 : 0;

    return 1;
}

/*
 * Apply a substitution to a single line (NUL-terminated, without newline).
 * Returns a heap-allocated result string.  Caller must free.
 * Returns NULL on allocation failure.
 */
static char *apply_subst(const char *line, const subst_t *s)
{
    size_t out_cap = strlen(line) * 2 + 256;
    char  *out     = malloc(out_cap);
    if (!out) return NULL;

    size_t out_len  = 0;
    size_t pat_len  = strlen(s->pattern);
    size_t rep_len  = strlen(s->replace);
    const char *pos = line;
    int replaced    = 0;

    while (*pos) {
        /* If we already replaced once and not global, copy remainder */
        if (replaced && !s->global) {
            size_t rest = strlen(pos);
            if (out_len + rest + 1 > out_cap) {
                out_cap = out_len + rest + 256;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); return NULL; }
                out = tmp;
            }
            memcpy(out + out_len, pos, rest);
            out_len += rest;
            break;
        }

        /* Try to match pattern at pos */
        if (pat_len > 0 && strncmp(pos, s->pattern, pat_len) == 0) {
            /* Ensure space for replacement */
            if (out_len + rep_len + 1 > out_cap) {
                out_cap = out_len + rep_len + 256;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); return NULL; }
                out = tmp;
            }
            memcpy(out + out_len, s->replace, rep_len);
            out_len  += rep_len;
            pos      += pat_len;
            replaced  = 1;
        } else {
            /* Copy one character */
            if (out_len + 2 > out_cap) {
                out_cap *= 2;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); return NULL; }
                out = tmp;
            }
            out[out_len++] = *pos++;
        }
    }

    out[out_len] = '\0';
    return out;
}

/* ---- backup + in-place transform ---------------------------------------- */

static int process_file(const char *path, const char *suffix,
                         subst_t *substs, int n_substs)
{
    /* Read file */
    FILE *fin = fopen(path, "r");
    if (!fin) {
        fprintf(stderr, "sed_inplace: open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    /* Collect all lines */
    char  **lines    = NULL;
    size_t  n_lines  = 0;
    size_t  cap      = 0;
    char    buf[MAX_LINE];

    while (fgets(buf, sizeof(buf), fin)) {
        /* Strip trailing newline for processing; we'll re-add it */
        size_t len = strlen(buf);
        int has_nl = (len > 0 && buf[len - 1] == '\n');
        if (has_nl) buf[len - 1] = '\0';

        if (n_lines >= cap) {
            cap = cap ? cap * 2 : 64;
            char **tmp = realloc(lines, cap * sizeof(*lines));
            if (!tmp) {
                fprintf(stderr, "sed_inplace: out of memory\n");
                fclose(fin);
                for (size_t i = 0; i < n_lines; i++) free(lines[i]);
                free(lines);
                return 1;
            }
            lines = tmp;
        }

        /* Apply substitutions */
        char *current = strdup(buf);
        if (!current) {
            fclose(fin);
            for (size_t i = 0; i < n_lines; i++) free(lines[i]);
            free(lines);
            return 1;
        }

        for (int j = 0; j < n_substs; j++) {
            char *next = apply_subst(current, &substs[j]);
            if (!next) {
                free(current);
                fclose(fin);
                for (size_t i = 0; i < n_lines; i++) free(lines[i]);
                free(lines);
                return 1;
            }
            free(current);
            current = next;
        }

        /* Re-attach newline marker by storing in a string ending with \n */
        size_t new_len = strlen(current);
        char *stored   = malloc(new_len + 3); /* current + \n + \0 */
        if (!stored) {
            free(current);
            fclose(fin);
            for (size_t i = 0; i < n_lines; i++) free(lines[i]);
            free(lines);
            return 1;
        }
        memcpy(stored, current, new_len);
        if (has_nl) {
            stored[new_len]     = '\n';
            stored[new_len + 1] = '\0';
        } else {
            stored[new_len] = '\0';
        }
        free(current);

        lines[n_lines++] = stored;
    }
    fclose(fin);

    /* Write backup */
    if (suffix && suffix[0] != '\0') {
        char backup[PATH_MAX];
        int r = snprintf(backup, sizeof(backup), "%s%s", path, suffix);
        if (r < 0 || (size_t)r >= sizeof(backup)) {
            fprintf(stderr, "sed_inplace: backup path too long for '%s'\n", path);
            for (size_t i = 0; i < n_lines; i++) free(lines[i]);
            free(lines);
            return 1;
        }

        FILE *fbk = fopen(backup, "w");
        if (!fbk) {
            fprintf(stderr, "sed_inplace: cannot create backup '%s': %s\n",
                    backup, strerror(errno));
            for (size_t i = 0; i < n_lines; i++) free(lines[i]);
            free(lines);
            return 1;
        }

        /* Re-read original for backup (we've already transformed lines) */
        FILE *forig = fopen(path, "r");
        if (forig) {
            char cbuf[65536];
            size_t nread;
            while ((nread = fread(cbuf, 1, sizeof(cbuf), forig)) > 0)
                fwrite(cbuf, 1, nread, fbk);
            fclose(forig);
        }
        fclose(fbk);
    }

    /* Write transformed content back to file */
    FILE *fout = fopen(path, "w");
    if (!fout) {
        fprintf(stderr, "sed_inplace: cannot write '%s': %s\n",
                path, strerror(errno));
        for (size_t i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return 1;
    }

    for (size_t i = 0; i < n_lines; i++) {
        fputs(lines[i], fout);
        free(lines[i]);
    }
    free(lines);
    fclose(fout);
    return 0;
}

/* ---- module handler ------------------------------------------------------ */

static int sed_inplace_handler(int argc, char **argv, int flag_index)
{
    /* Parse -i<suffix> or -i <suffix> */
    const char *suffix = "";
    const char *flag   = argv[flag_index];

    /* flag is "-i" possibly with suffix immediately attached */
    if (flag[0] == '-' && flag[1] == 'i') {
        suffix = flag + 2; /* may be empty string for bare -i */
        /* If suffix is empty and next arg doesn't start with '-', treat as suffix
         * only if it doesn't look like an expression or file */
        if (suffix[0] == '\0' && flag_index + 1 < argc &&
            argv[flag_index + 1][0] != '-') {
            /* Ambiguous: POSIX sed -i doesn't take a separate argument for suffix.
             * GNU sed -i'' vs -i SUFFIX.  We follow GNU: if the next token is not
             * a sed expression (not starting with s, d, p, etc.) use it as suffix.
             * Conservatively, only treat as suffix if it contains a '.'. */
            const char *next = argv[flag_index + 1];
            if (strchr(next, '.') && next[0] != 's') {
                suffix = next;
                /* We can't shift argc/argv here; the caller owns argv */
            }
        }
    }

    /* Collect -e expressions and file operands */
    subst_t  substs[64];
    int      n_substs = 0;
    int      in_files = 0;
    int      ret      = 0;

    /* Collect -e EXPR */
    for (int i = 1; i < argc && n_substs < 64; i++) {
        if (i == flag_index) continue;
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            subst_t s;
            if (parse_subst(argv[i + 1], &s)) {
                substs[n_substs++] = s;
            }
            i++; /* skip expression */
            continue;
        }
        /* Bare expression (not starting with - and contains 's') might be script */
        if (argv[i][0] != '-') {
            in_files = i;
            break;
        }
    }

    /* If no -e, check for a bare script argument before file list */
    if (n_substs == 0 && in_files > 0) {
        subst_t s;
        if (parse_subst(argv[in_files], &s)) {
            substs[n_substs++] = s;
            in_files++;
        }
    }

    if (in_files == 0 || in_files >= argc) {
        fprintf(stderr, "sed_inplace: no input files specified\n");
        for (int i = 0; i < n_substs; i++) {
            free(substs[i].pattern);
            free(substs[i].replace);
        }
        return 1;
    }

    /* Process each file */
    for (int i = in_files; i < argc; i++) {
        if (i == flag_index) continue;
        if (argv[i][0] == '-') continue; /* skip stray flags */
        if (process_file(argv[i], suffix, substs, n_substs) != 0)
            ret = 1;
    }

    for (int i = 0; i < n_substs; i++) {
        free(substs[i].pattern);
        free(substs[i].replace);
    }

    return ret;
}

/* ---- module metadata ----------------------------------------------------- */

static const char *sed_inplace_flags[] = {
    "-i",
    NULL
};

static silex_module_t sed_inplace_module = {
    .api_version = SILEX_MODULE_API_VERSION,
    .libc        = SILEX_LIBC_NAME,
    .tool_name   = "sed",
    .module_name = "sed_inplace",
    .description = "sed -i SUFFIX: in-place editing with optional backup suffix",
    .extra_flags = sed_inplace_flags,
    .handler     = sed_inplace_handler,
};

silex_module_t *silex_module_init(void)
{
    return &sed_inplace_module;
}
