/* vcsignore.c — .gitignore parser and VCS-aware file filter */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "vcsignore.h"

#include <ctype.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Hardcoded VCS / noise skip list                                    */
/* ------------------------------------------------------------------ */

/* Directory names that are always skipped regardless of .gitignore */
static const char *const SKIP_DIRS[] = {
    ".git", ".svn", ".hg", ".bzr",
    "node_modules", "__pycache__", ".tox", ".mypy_cache",
    ".pytest_cache", ".cache", ".cargo",
    NULL
};

/* File name/glob patterns that are always skipped */
static const char *const SKIP_FILES[] = {
    ".DS_Store", "Thumbs.db",
    NULL
};

/* Extension-based noise: skip if basename ends with these */
static const char *const SKIP_EXTS[] = {
    ".o", ".a", ".so", ".pyc", ".pyo", ".class",
    ".exe", ".dll", ".obj",
    NULL
};

/* ------------------------------------------------------------------ */
/* Rule storage                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *pat;       /* pattern (stripped of leading !, leading /, trailing /) */
    int    negate;    /* rule starts with '!' — un-ignore */
    int    rooted;    /* rule starts with '/' — anchored at root */
    int    dir_only;  /* rule ends with '/'  — directories only */
    int    has_slash; /* rule contains '/' other than leading/trailing */
} vcs_rule_t;

struct vcsignore {
    vcs_rule_t *rules;
    int         nrules;
    int         cap;
};

/* ------------------------------------------------------------------ */
/* Glob matching with ** support                                      */
/* ------------------------------------------------------------------ */

/* Forward declaration (recursive function) */
static int vcs_glob_match(const char *pat, const char *path);

/*
 * Match path (forward-slash delimited, relative) against gitignore pat.
 * Handles:
 *   STARSTAR/foo   - foo at any depth
 *   foo/STARSTAR   - everything under foo/
 *   a/STARSTAR/b   - a/b, a/x/b, a/x/y/b, ...
 *   *.c / ?.h      - standard fnmatch globs
 */
static int vcs_glob_match(const char *pat, const char *path)
{
    /* STARSTAR/rest: try rest at every path depth */
    if (strncmp(pat, "**/", 3) == 0) {
        const char *p = path;
        for (;;) {
            if (vcs_glob_match(pat + 3, p)) return 1;
            p = strchr(p, '/');
            if (!p) break;
            p++;
        }
        return 0;
    }

    /* foo/STARSTAR: everything whose path starts with foo/ */
    size_t plen = strlen(pat);
    if (plen >= 3 && strcmp(pat + plen - 3, "/**") == 0) {
        size_t prefix = plen - 3;
        if (strncmp(path, pat, prefix) == 0) {
            char c = path[prefix];
            if (c == '/' || c == '\0') return 1;
        }
        return 0;
    }

    /* a/STARSTAR/b: split on first occurrence of slash-double-star-slash */
    const char *ds = strstr(pat, "/**/");
    if (ds) {
        size_t prefix_len = (size_t)(ds - pat);
        const char *rest_pat = ds + 4; /* skip the four chars */

        /* path must start with the prefix part */
        if (strncmp(path, pat, prefix_len) != 0) return 0;
        if (path[prefix_len] == '\0') return 0;
        const char *p = path + prefix_len + 1; /* skip slash */

        /* double-star matches zero or more path components; try each */
        for (;;) {
            if (vcs_glob_match(rest_pat, p)) return 1;
            p = strchr(p, '/');
            if (!p) break;
            p++;
        }
        return 0;
    }

    /* No double-star: use fnmatch. Patterns with slash get FNM_PATHNAME. */
    if (strchr(pat, '/'))
        return fnmatch(pat, path, FNM_PATHNAME) == 0;
    else
        return fnmatch(pat, path, 0) == 0;
}

/* ------------------------------------------------------------------ */
/* Rule matching                                                      */
/* ------------------------------------------------------------------ */

static int rule_matches(const vcs_rule_t *rule,
                        const char *relpath, int is_dir)
{
    if (rule->dir_only && !is_dir)
        return 0;

    /* Determine match subject */
    if (rule->has_slash || rule->rooted) {
        /* Match against full relative path */
        return vcs_glob_match(rule->pat, relpath);
    } else {
        /* Match against basename only */
        const char *base = strrchr(relpath, '/');
        base = base ? base + 1 : relpath;
        return fnmatch(rule->pat, base, 0) == 0;
    }
}

/* ------------------------------------------------------------------ */
/* Load helpers                                                       */
/* ------------------------------------------------------------------ */

static vcsignore_t *vcsignore_alloc(void)
{
    vcsignore_t *ign = calloc(1, sizeof(vcsignore_t));
    if (!ign) return NULL;
    ign->cap   = 64;
    ign->rules = malloc((size_t)ign->cap * sizeof(vcs_rule_t));
    if (!ign->rules) { free(ign); return NULL; }
    return ign;
}

static int vcsignore_add(vcsignore_t *ign,
                         const char *pat, size_t plen,
                         int negate, int rooted, int dir_only)
{
    if (ign->nrules >= ign->cap) {
        int newcap = ign->cap * 2;
        vcs_rule_t *r2 = realloc(ign->rules,
                                 (size_t)newcap * sizeof(vcs_rule_t));
        if (!r2) return -1;
        ign->rules = r2;
        ign->cap   = newcap;
    }

    vcs_rule_t *r = &ign->rules[ign->nrules];
    r->pat = malloc(plen + 1);
    if (!r->pat) return -1;
    memcpy(r->pat, pat, plen);
    r->pat[plen] = '\0';

    r->negate    = negate;
    r->rooted    = rooted;
    r->dir_only  = dir_only;

    /* has_slash: contains '/' other than leading or trailing position */
    r->has_slash = (strchr(r->pat, '/') != NULL);

    ign->nrules++;
    return 0;
}

/* Parse one line from a .gitignore file into ign. */
static void parse_gitignore_line(vcsignore_t *ign, const char *line)
{
    /* Strip trailing newline / carriage return */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
        len--;

    /* Skip blank lines and comments */
    if (len == 0 || line[0] == '#') return;

    /* Trailing spaces (not escaped) are ignored */
    while (len > 0 && line[len-1] == ' ' && (len < 2 || line[len-2] != '\\'))
        len--;
    if (len == 0) return;

    const char *p = line;
    int negate = 0, rooted = 0, dir_only = 0;

    /* Negation */
    if (*p == '!') { negate = 1; p++; len--; }

    /* Leading slash: rooted */
    if (len > 0 && *p == '/') { rooted = 1; p++; len--; }

    /* Trailing slash: directory-only */
    if (len > 0 && p[len-1] == '/') { dir_only = 1; len--; }

    if (len == 0) return;

    vcsignore_add(ign, p, len, negate, rooted, dir_only);
}

/* Load rules from a .gitignore file into ign. */
static void load_gitignore_file(vcsignore_t *ign, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char buf[4096];
    while (fgets(buf, (int)sizeof(buf), fp))
        parse_gitignore_line(ign, buf);

    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

vcsignore_t *vcsignore_load(const char *dir)
{
    vcsignore_t *ign = vcsignore_alloc();
    if (!ign) return NULL;

    /* Walk from dir up to filesystem root, loading .gitignore files.
     * Rules from deeper directories take precedence (added last → last-wins). */
    char paths[32][PATH_MAX];
    int  ndirs = 0;

    char cur[PATH_MAX];
    if (!dir || dir[0] == '\0') {
        cur[0] = '.'; cur[1] = '\0';
    } else {
        size_t dlen = strlen(dir);
        if (dlen >= PATH_MAX) dlen = PATH_MAX - 1;
        memcpy(cur, dir, dlen);
        cur[dlen] = '\0';
        /* Remove trailing slash */
        while (dlen > 1 && cur[dlen-1] == '/')
            cur[--dlen] = '\0';
    }

    /* Collect directories from current up to root (stop at .git boundary) */
    while (ndirs < 32) {
        memcpy(paths[ndirs], cur, strlen(cur) + 1);
        ndirs++;

        /* Stop if this dir contains .git (we're at the repo root) */
        char gitdir[PATH_MAX];
        size_t curlen = strlen(cur);
        int need_slash = (cur[curlen - 1] != '/');
        int gsz = snprintf(gitdir, sizeof(gitdir), "%s%s.git",
                           cur, need_slash ? "/" : "");
        struct stat st;
        if (gsz > 0 && (size_t)gsz < sizeof(gitdir) &&
            stat(gitdir, &st) == 0 && S_ISDIR(st.st_mode))
            break;

        /* Move to parent */
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = '\0';
    }

    /* Load .gitignore from root → current (so deeper rules win) */
    for (int i = ndirs - 1; i >= 0; i--) {
        char gi[PATH_MAX];
        size_t plen = strlen(paths[i]);
        int need_slash = (paths[i][plen - 1] != '/');
        int gsz = snprintf(gi, sizeof(gi), "%s%s.gitignore",
                           paths[i], need_slash ? "/" : "");
        if (gsz > 0 && (size_t)gsz < sizeof(gi))
            load_gitignore_file(ign, gi);
    }

    return ign;
}

int vcsignore_match(const vcsignore_t *ign, const char *relpath, int is_dir)
{
    if (!ign || !relpath) return 0;

    /* Quick hardcoded check first (basename) */
    if (vcsignore_skip_name(
            (strrchr(relpath, '/') ? strrchr(relpath, '/') + 1 : relpath),
            is_dir))
        return 1;

    /* Walk rules; last match wins (gitignore semantics) */
    int ignored = 0;
    for (int i = 0; i < ign->nrules; i++) {
        if (rule_matches(&ign->rules[i], relpath, is_dir))
            ignored = ign->rules[i].negate ? 0 : 1;
    }
    return ignored;
}

int vcsignore_skip_name(const char *name, int is_dir)
{
    if (!name) return 0;

    if (is_dir) {
        for (int i = 0; SKIP_DIRS[i]; i++)
            if (strcmp(name, SKIP_DIRS[i]) == 0) return 1;
    } else {
        for (int i = 0; SKIP_FILES[i]; i++)
            if (strcmp(name, SKIP_FILES[i]) == 0) return 1;

        /* Extension check */
        size_t nlen = strlen(name);
        for (int i = 0; SKIP_EXTS[i]; i++) {
            size_t elen = strlen(SKIP_EXTS[i]);
            if (nlen >= elen &&
                strcmp(name + nlen - elen, SKIP_EXTS[i]) == 0)
                return 1;
        }
    }
    return 0;
}

void vcsignore_free(vcsignore_t *ign)
{
    if (!ign) return;
    for (int i = 0; i < ign->nrules; i++)
        free(ign->rules[i].pat);
    free(ign->rules);
    free(ign);
}
