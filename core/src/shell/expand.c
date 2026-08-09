/* expand.c — word expansion: parameter, arithmetic, and glob */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "expand.h"
#include "shell.h"
#include "../util/strbuf.h"
#include "../util/arena.h"
#include "../util/charclass.h"
#include "../util/intern.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Forward declarations */
static void expand_into(shell_ctx_t *sh, const char *word, strbuf_t *out,
                        int in_dquote);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static int is_special_var(char c)
{
    return c == '@' || c == '*' || c == '#' || c == '?' ||
           c == '-' || c == '$' || c == '!' || c == '0';
}

/* Return the value of $name from shell_ctx (vars + positional + specials) */
static const char *sh_getvar(shell_ctx_t *sh, const char *name)
{
    /* Special variables */
    if (name[0] != '\0' && name[1] == '\0') {
        char buf[32];
        switch (name[0]) {
        case '?':
            snprintf(buf, sizeof(buf), "%d", sh->last_exit);
            return arena_strdup(sh->scratch, buf);
        case '$':
            /* Cached at init, not getpid(): POSIX $$ is the main shell's PID and
             * must not change in a subshell fork. See shell.h. */
            snprintf(buf, sizeof(buf), "%ld", (long)sh->shell_pid);
            return arena_strdup(sh->scratch, buf);
        case '!':
            if (sh->last_bg_pid == 0) return "";
            snprintf(buf, sizeof(buf), "%ld", (long)sh->last_bg_pid);
            return arena_strdup(sh->scratch, buf);
        case '#':
            snprintf(buf, sizeof(buf), "%d", sh->positional_n);
            return arena_strdup(sh->scratch, buf);
        case '-': {
            /* Return current option flags */
            strbuf_t sb;
            sb_init(&sb, 8);
            if (sh->opt_a) sb_appendc(&sb, 'a');
            if (sh->opt_e) sb_appendc(&sb, 'e');
            if (sh->opt_u) sb_appendc(&sb, 'u');
            if (sh->opt_x) sb_appendc(&sb, 'x');
            if (sh->opt_f) sb_appendc(&sb, 'f');
            if (sh->opt_n) sb_appendc(&sb, 'n');
            if (sh->opt_m) sb_appendc(&sb, 'm');
            if (sh->opt_C) sb_appendc(&sb, 'C');
            char *r = arena_strdup(sh->scratch, sb_str(&sb));
            sb_free(&sb);
            return r;
        }
        case '0':
            return sh->script_name ? sh->script_name : "";
        case '@':
        case '*': {
            /* Join all positionals: $* uses first char of IFS, $@ uses space */
            strbuf_t sb;
            sb_init(&sb, 64);
            char sep;
            if (name[0] == '*') {
                const char *ifs = sh_getvar(sh, "IFS");
                sep = (ifs && ifs[0]) ? ifs[0] : ' ';
            } else {
                sep = ' ';
            }
            for (int i = 0; i < sh->positional_n; i++) {
                if (i > 0) sb_appendc(&sb, sep);
                sb_append(&sb, sh->positional[i]);
            }
            char *r = arena_strdup(sh->scratch, sb_str(&sb));
            sb_free(&sb);
            return r;
        }
        default:
            break;
        }
    }

    /* $1 .. $9 and ${10}, ${11}, ... */
    if (name[0] >= '1' && name[0] <= '9') {
        if (name[1] == '\0') {
            int idx = name[0] - '0';
            if (idx <= sh->positional_n)
                return sh->positional[idx - 1];
            return NULL;
        }
        /* multi-digit: ${10}, ${11}, ... */
        int idx = atoi(name);
        if (idx > 0 && idx <= sh->positional_n)
            return sh->positional[idx - 1];
        return NULL;
    }

    /* LINENO special */
    if (strcmp(name, "LINENO") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0");
        return arena_strdup(sh->scratch, buf);
    }

    return vars_get(&sh->vars, name);
}

/* -------------------------------------------------------------------------
 * Tilde expansion
 * Returns newly-malloc'd string or NULL if no tilde
 * ------------------------------------------------------------------------- */

static char *expand_tilde(shell_ctx_t *sh, const char *word, int in_assignment)
{
    if (word[0] != '~')
        return NULL;

    const char *rest = word + 1;
    /* Tilde expands if followed by:
     *  - / (always)
     *  - : (only in assignment context for PATH-like values)
     *  - end of string (always) */
    const char *slash = strchr(rest, '/');
    const char *colon = in_assignment ? strchr(rest, ':') : NULL;
    const char *delim = NULL;

    /* Find first delimiter (/ or :) */
    if (slash && colon) delim = (slash < colon) ? slash : colon;
    else if (slash) delim = slash;
    else if (colon) delim = colon;

    char username[256];
    size_t ulen = delim ? (size_t)(delim - rest) : strlen(rest);

    const char *home = NULL;

    if (ulen == 0) {
        /* ~  or ~/... */
        home = sh_getvar(sh, "HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        if (!home) home = "";
    } else {
        if (ulen >= sizeof(username))
            return NULL;
        memcpy(username, rest, ulen);
        username[ulen] = '\0';
        struct passwd *pw = getpwnam(username);
        if (!pw) return NULL;
        home = pw->pw_dir;
    }

    /* Build result */
    strbuf_t sb;
    sb_init(&sb, 128);
    sb_append(&sb, home);
    if (delim) sb_append(&sb, delim);  /* Append from delimiter onwards (/ or :) */
    char *result = strdup(sb_str(&sb));
    sb_free(&sb);
    return result;
}

/* -------------------------------------------------------------------------
 * Pattern matching helpers for ${VAR#pat}, ${VAR%pat}, ${VAR/pat/repl}
 * ------------------------------------------------------------------------- */

/* Match pat anchored at the start of str; return length of match or -1 */
static int match_prefix(const char *str, const char *pat)
{
    /* Try lengths from longest to shortest for ## */
    size_t slen = strlen(str);
    char *tmp = malloc(slen + 1);
    if (!tmp) return -1;
    int result = -1;
    for (size_t len = slen; ; len--) {
        memcpy(tmp, str, len);
        tmp[len] = '\0';
        if (fnmatch(pat, tmp, 0) == 0) {
            result = (int)len;
            break;
        }
        if (len == 0) break;
    }
    free(tmp);
    return result;
}

static int match_prefix_shortest(const char *str, const char *pat)
{
    size_t slen = strlen(str);
    char *tmp = malloc(slen + 1);
    if (!tmp) return -1;
    int result = -1;
    for (size_t len = 0; len <= slen; len++) {
        memcpy(tmp, str, len);
        tmp[len] = '\0';
        if (fnmatch(pat, tmp, 0) == 0) {
            result = (int)len;
            break;
        }
    }
    free(tmp);
    return result;
}

/* Match pat anchored at the end of str; return start offset of match or -1 */
static int match_suffix(const char *str, const char *pat)
{
    /* Longest: try from smallest start offset */
    size_t slen = strlen(str);
    for (size_t off = 0; off <= slen; off++) {
        if (fnmatch(pat, str + off, 0) == 0)
            return (int)off;
    }
    return -1;
}

static int match_suffix_shortest(const char *str, const char *pat)
{
    /* Shortest: try from largest start offset */
    size_t slen = strlen(str);
    for (size_t off = slen; ; off--) {
        if (fnmatch(pat, str + off, 0) == 0)
            return (int)off;
        if (off == 0) break;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Word analysis helpers: check original token text for unquoted chars
 * ------------------------------------------------------------------------- */

/* Returns 1 if 'word' contains unquoted '$' or backtick (i.e., the result of
 * expand_word() may differ from the literal text and should be IFS-split). */
static int has_unquoted_expansion(const char *word)
{
    int in_sq = 0, in_dq = 0;
    for (const char *p = word; *p; p++) {
        if (*p == '\\' && !in_sq) { p++; if (!*p) break; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; continue; }
        /* Expansion is "unquoted" (subject to IFS splitting) only outside all quotes */
        if (!in_sq && !in_dq && (*p == '$' || *p == '`')) return 1;
    }
    return 0;
}

/* Returns 1 if 'word' contains unquoted glob chars (* ? [) outside quotes. */
static int has_unquoted_glob(const char *word)
{
    int in_sq = 0, in_dq = 0;
    for (const char *p = word; *p; p++) {
        if (*p == '\\' && !in_sq) { p++; if (!*p) break; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; continue; }
        if (in_sq || in_dq) continue;

        if (*p == '*' || *p == '?')
            return 1;

        if (*p == '[') {
            /* A '[' only opens a bracket expression if something closes it in
             * the SAME word. A bare '[' is the test command, and treating it as
             * a glob metacharacter meant every `if [ ... ]` and `while [ ... ]`
             * ran a full glob() over the working directory -- an opendir plus
             * two getdents64 per conditional. GLOB_NOCHECK handed back the
             * literal '[', so the answer was right and the cost was enormous:
             *
             *   while [ $i -lt 2000 ]      169 ms
             *   while test $i -lt 2000       9 ms   <- the very same builtin
             *
             * and it scaled with the size of the directory: 500 iterations cost
             * 13 ms in a directory of 10 files and 179 ms in one of 2000.
             * Configure scripts are mostly conditionals, run against a source
             * tree -- the worst case on both counts.
             */
            int sq = 0, dq = 0;
            for (const char *q = p + 1; *q; q++) {
                if (*q == '\\' && !sq) { q++; if (!*q) break; continue; }
                if (*q == '\'' && !dq) { sq = !sq; continue; }
                if (*q == '"'  && !sq) { dq = !dq; continue; }
                if (!sq && !dq && *q == ']')
                    return 1;               /* a complete bracket expression */
            }
            /* No closing ']', so not a glob. Keep looking for a * or ?. */
        }
    }
    return 0;
}

/* Returns 1 if a field lying within a guard-stripped word (with `prot` map)
 * contains an active glob metacharacter (* ? or a complete [...]) that is
 * UNPROTECTED -- i.e. came from unquoted text/expansion. `s` points into
 * `base`; prot[i] marks byte i of base as protected (was inside quotes), and
 * may be NULL (nothing protected). This lets a word that MIXES a quoted region
 * with an unquoted pattern -- e.g. `"$dir"/$pat` with pat='*.mm' -- still glob
 * the unquoted `*`, while a quoted "*" (prot set) never globs. The '[' needs a
 * closing ']' for the same reason as has_unquoted_glob (a bare '[' is `test`,
 * not a glob, and globbing it would opendir the whole cwd per conditional). */
static int field_has_unprotected_glob(const char *s, const char *prot,
                                      const char *base)
{
    for (const char *p = s; *p; p++) {
        int protd = prot && prot[(size_t)(p - base)];
        if (protd) continue;
        if (*p == '*' || *p == '?')
            return 1;
        if (*p == '[') {
            for (const char *q = p + 1; *q; q++)
                if (*q == ']')
                    return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Parameter expansion: ${...} parsing
 * name_start points to char after '$' or after '{'
 * Returns arena-allocated expanded string
 * ------------------------------------------------------------------------- */

/* Decode/encode the internal reserved-byte escaping (see PP_ESC /
 * pp_escape_append, defined below). Forward-declared here because ${var=word}
 * must store the DECODED assigned value, and expand_braced()'s data-return paths
 * must ENCODE the value they hand back so a literal reserved byte in it is not
 * re-consumed by the caller's field-split decode. */
static char *pp_decode(arena_t *a, const char *s);
static void  pp_escape_append(strbuf_t *out, const char *s);

/* Return a ${...}-expansion DATA result (a variable's value, a trimmed value, a
 * substring -- NOT a word-part that already went through emit_data). When the
 * enclosing word will be field-split (emit_guards), reserved bytes are encoded
 * so the caller's decode restores them; otherwise the value is returned raw. */
static char *braced_ret(shell_ctx_t *sh, const char *s)
{
    if (!s) s = "";
    if (!sh->emit_guards)
        return arena_strdup(sh->scratch, s);
    strbuf_t sb;
    sb_init(&sb, strlen(s) + 8);
    pp_escape_append(&sb, s);
    char *r = arena_strdup(sh->scratch, sb_str(&sb));
    sb_free(&sb);
    return r;
}

static char *braced_ret_n(shell_ctx_t *sh, const char *s, size_t n)
{
    char *tmp = arena_strndup(sh->scratch, s, n);
    return braced_ret(sh, tmp);
}

static char *expand_braced_body(shell_ctx_t *sh, const char *body, int in_dquote);

/*
 * expand_braced: parse and expand a ${...} body.
 *
 * Inside a ${...}, quoting is processed normally EVEN within a here-document
 * body -- only the surrounding here-doc TEXT treats quotes literally. So a
 * here-doc's `${U-"1"}` yields `1` (the default's quotes are removed) and
 * `${S#"se"}` trims by the unquoted pattern, matching dash. The here-doc is
 * still a NON-split context, though, so $@/$* must join: we set in_assign for
 * the duration, which the $@/$* branch treats as "join".
 */
static char *expand_braced(shell_ctx_t *sh, const char *body, int in_dquote)
{
    int save_h = sh->in_heredoc, save_a = sh->in_assign;
    if (save_h) {
        /* A here-doc body carries no real double quotes (they are literal text),
         * so its ${...} interior is an UNQUOTED context: `"1"` in ${U-"1"} opens
         * and closes normally. Pass in_dquote=0, join $@/$* (non-split), and let
         * quotes be processed (in_heredoc off). */
        sh->in_assign = 1;
        sh->in_heredoc = 0;
        in_dquote = 0;
    }
    char *r = expand_braced_body(sh, body, in_dquote);
    sh->in_heredoc = save_h;
    sh->in_assign  = save_a;
    return r;
}

/*
 * expand_braced_body: the actual ${...} parser/expander.
 * 'body' is the content between { and }, e.g. "VAR:-default" or "#VAR".
 * in_dquote is the quoting of the surrounding context: it carries into the
 * word part of ${var-WORD} etc., so `"${1+$@}"` treats $@ as a quoted "$@"
 * (separate fields) while `${var=$*}` treats $* as unquoted (joined).
 */
static char *expand_braced_body(shell_ctx_t *sh, const char *body, int in_dquote)
{
    /* Empty expansion ${} is an error */
    if (body[0] == '\0') {
        fprintf(stderr, "silex: bad substitution\n");
        exit(2);
    }

    /* Bash indirect (${!name}) and prefix (${!name@}, ${!name*}) expansion are
     * not POSIX and silex does not implement them. Reject them as a bad
     * substitution, exactly as dash does, instead of silently mis-parsing
     * `!name` as the `!` special parameter followed by leftover text (which
     * produced a stray last-background-PID and made modernish wrongly detect the
     * VARPREFIX capability). `${!}` (the last-bg-PID special parameter) and
     * `${!:-x}` stay valid: only `!` directly followed by a name char, @, or *
     * is rejected. */
    if (body[0] == '!' &&
        (is_name_char((unsigned char)body[1]) || body[1] == '@' || body[1] == '*')) {
        fprintf(stderr, "silex: bad substitution\n");
        exit(2);
    }

    /* ${#VAR} — length */
    if (body[0] == '#' && body[1] != '\0' && body[1] != '}') {
        const char *varname = body + 1;
        const char *val = sh_getvar(sh, varname);
        if (!val && sh->opt_u) {
            fprintf(stderr, "silex: %s: unbound variable\n", varname);
            exit(1);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", val ? strlen(val) : (size_t)0);
        return arena_strdup(sh->scratch, buf);
    }

    /* Find operator position.
     * Varname chars are [A-Za-z0-9_].  Single-char special vars ($?, $#, etc.)
     * are also allowed as bare ${?}, but must not consume operator characters
     * like # or % which follow the varname in ${VAR#pat}. */
    const char *p = body;
    if (is_special_var(body[0])) {
        /* Single-char special variable: advance exactly one char */
        p++;
    } else {
        /* Normal identifier: [A-Za-z_][A-Za-z0-9_]* */
        while (is_name_char((unsigned char)*p))
            p++;
    }

    size_t namelen = (size_t)(p - body);
    /* Was a VLA sized by the name length in ${...}, i.e. by the input. Names
     * are short in practice, so use a fixed buffer and reject anything absurd
     * rather than putting an attacker-controlled size on the stack. */
    char varname[256];
    if (namelen >= sizeof(varname)) {
        fprintf(stderr, "silex: variable name too long\n");
        return NULL;
    }
    memcpy(varname, body, namelen);
    varname[namelen] = '\0';

    const char *val = sh_getvar(sh, varname);

    /* No operator: plain ${VAR} */
    if (*p == '\0') {
        return braced_ret(sh, val);
    }

    /* ${VAR:-word}, ${VAR:+word}, ${VAR:=word}, ${VAR:?word} */
    int colon = (*p == ':');
    if (colon) p++;

    /* ${VAR:offset}, ${VAR:offset:length} — substring (ksh/bash extension)
     * Triggered when: digit after ':', OR space(s) then '-' digit (${x: -3}). */
    if (colon) {
        const char *q = p;
        int had_space = 0;
        while (*q == ' ') { had_space = 1; q++; }
        if (is_digit((unsigned char)*q) ||
            (had_space && *q == '-' && is_digit((unsigned char)*(q + 1)))) {
            p = q;
            int neg_off = (*p == '-');
            if (neg_off) p++;
            long off = 0;
            while (is_digit((unsigned char)*p))
                off = off * 10 + ((unsigned char)*p++ - '0');
            if (neg_off) off = -off;
            long sub_len = 0;
            int has_len = 0;
            if (*p == ':') {
                p++;
                has_len = 1;
                int neg_len = (*p == '-');
                if (neg_len) p++;
                long ll = 0;
                while (is_digit((unsigned char)*p))
                    ll = ll * 10 + ((unsigned char)*p++ - '0');
                sub_len = neg_len ? -ll : ll;
            }
            const char *s = val ? val : "";
            long slen = (long)strlen(s);
            if (off < 0) { off += slen; if (off < 0) off = 0; }
            if (off > slen) off = slen;
            long avail = slen - off;
            if (has_len) {
                if (sub_len >= 0 && sub_len < avail) avail = sub_len;
                else if (sub_len < 0) avail += sub_len;  /* trim from end */
            }
            if (avail <= 0) return arena_strdup(sh->scratch, "");
            return braced_ret_n(sh, s + off, (size_t)avail);
        }
    }

    char op = *p;
    if (op == '-' || op == '+' || op == '=' || op == '?') {
        p++; /* skip operator char */
        const char *word_part = p;
        int unset = (val == NULL);
        int empty  = (val != NULL && val[0] == '\0');

        int condition = colon ? (unset || empty) : unset;

        switch (op) {
        case '-':
            if (condition) {
                /* Expand and return word_part */
                strbuf_t sb;
                sb_init(&sb, 64);
                { int sj_ = sh->pp_join_unquoted; int wd_ = sh->pp_word_dq;
                  sh->pp_join_unquoted = !in_dquote; sh->pp_word_dq = in_dquote;
                  expand_into(sh, word_part, &sb, in_dquote);
                  sh->pp_join_unquoted = sj_; sh->pp_word_dq = wd_; }
                char *r = arena_strdup(sh->scratch, sb_str(&sb));
                sb_free(&sb);
                return r;
            }
            return braced_ret(sh, val);

        case '+':
            if (!condition) {
                /* Variable is set (and non-empty if colon) — expand word */
                strbuf_t sb;
                sb_init(&sb, 64);
                { int sj_ = sh->pp_join_unquoted; int wd_ = sh->pp_word_dq;
                  sh->pp_join_unquoted = !in_dquote; sh->pp_word_dq = in_dquote;
                  expand_into(sh, word_part, &sb, in_dquote);
                  sh->pp_join_unquoted = sj_; sh->pp_word_dq = wd_; }
                char *r = arena_strdup(sh->scratch, sb_str(&sb));
                sb_free(&sb);
                return r;
            }
            return arena_strdup(sh->scratch, "");

        case '=':
            if (condition) {
                strbuf_t sb;
                sb_init(&sb, 64);
                { int sj_ = sh->pp_join_unquoted; int wd_ = sh->pp_word_dq;
                  sh->pp_join_unquoted = !in_dquote; sh->pp_word_dq = in_dquote;
                  expand_into(sh, word_part, &sb, in_dquote);
                  sh->pp_join_unquoted = sj_; sh->pp_word_dq = wd_; }
                const char *newval = sb_str(&sb);
                /* The expansion may carry reserved-byte escapes (e.g. `$*` with
                 * a control byte in a splitting word). The VARIABLE must receive
                 * the decoded, literal value; the RETURNED text keeps the escapes
                 * so the caller's field splitting/decoding sees them. */
                vars_set(&sh->vars, varname, pp_decode(sh->scratch, newval));
                char *r = arena_strdup(sh->scratch, newval);
                sb_free(&sb);
                return r;
            }
            return braced_ret(sh, val);

        case '?':
            if (condition) {
                strbuf_t sb;
                sb_init(&sb, 64);
                { int sj_ = sh->pp_join_unquoted; int wd_ = sh->pp_word_dq;
                  sh->pp_join_unquoted = !in_dquote; sh->pp_word_dq = in_dquote;
                  expand_into(sh, word_part, &sb, in_dquote);
                  sh->pp_join_unquoted = sj_; sh->pp_word_dq = wd_; }
                fprintf(stderr, "%s: %s\n", varname,
                        sb_len(&sb) > 0 ? sb_str(&sb) : "parameter null or not set");
                sb_free(&sb);
                exit(1);
            }
            return braced_ret(sh, val);

        default:
            break;
        }
    }

    /* Back up if colon but no recognised operator */
    if (colon) p--;

    /* ${VAR#pat}, ${VAR##pat}, ${VAR%pat}, ${VAR%%pat} */
    if (op == '#') {
        p++;  /* skip the operator '#' */
        int greedy = (*p == '#');
        if (greedy) p++;
        /* The pattern is expanded (tilde/param/command/arith) and quote-aware:
         * quoted metacharacters are literal, so `${t%"${t#a}"}` strips the
         * literal text the nested expansion yields. Using the raw text left
         * quotes and nested expansions unprocessed -- modernish's FTL_PSUB2. */
        const char *pat = expand_word_pattern(sh, p);
        const char *s   = val ? val : "";
        int off = greedy ? match_prefix(s, pat) : match_prefix_shortest(s, pat);
        if (off < 0)
            return braced_ret(sh, s);
        return braced_ret(sh, s + off);
    }

    if (op == '%') {
        p++;  /* skip the operator '%' */
        int greedy = (*p == '%');
        if (greedy) p++;
        /* Quote-aware, expanded pattern -- see the '#' operator above. */
        const char *pat = expand_word_pattern(sh, p);
        const char *s   = val ? val : "";
        int off = greedy ? match_suffix(s, pat) : match_suffix_shortest(s, pat);
        if (off < 0)
            return braced_ret(sh, s);
        /* Remove suffix starting at off */
        return braced_ret_n(sh, s, (size_t)off);
    }

    /* ${VAR/pat/repl}, ${VAR//pat/repl} */
    if (op == '/') {
        p++;  /* skip the operator '/' */
        int global = (*p == '/');
        if (global) p++;
        /* Split on the next unescaped '/' */
        const char *pat_start = p;
        const char *sep = strchr(p, '/');
        char pat_buf[512];
        const char *repl = "";
        if (sep) {
            size_t plen = (size_t)(sep - pat_start);
            if (plen >= sizeof(pat_buf)) plen = sizeof(pat_buf) - 1;
            memcpy(pat_buf, pat_start, plen);
            pat_buf[plen] = '\0';
            repl = sep + 1;
        } else {
            size_t plen = strlen(pat_start);
            if (plen >= sizeof(pat_buf)) plen = sizeof(pat_buf) - 1;
            memcpy(pat_buf, pat_start, plen);
            pat_buf[plen] = '\0';
        }

        const char *s = val ? val : "";
        strbuf_t sb;
        sb_init(&sb, 128);
        size_t slen = strlen(s);

        /* This used to declare `char tmp[mlen + 1]` inside the inner loop -- a
         * variable-length array sized by the remaining length of the subject.
         * A large variable therefore put an unbounded allocation on the stack,
         * once per iteration, with no way to detect failure. Allocate the
         * scratch buffer once on the heap instead. */
        char *tmp = malloc(slen + 1);
        if (!tmp) {
            sb_free(&sb);
            return braced_ret(sh, s);
        }

        size_t i = 0;
        int replaced = 0;
        while (i <= slen) {
            /* Try matching at position i */
            int matched = 0;
            for (size_t mlen = slen - i; ; mlen--) {
                memcpy(tmp, s + i, mlen);
                tmp[mlen] = '\0';
                if (fnmatch(pat_buf, tmp, 0) == 0) {
                    sb_append(&sb, repl);
                    i += mlen;
                    matched = 1;
                    replaced = 1;
                    break;
                }
                if (mlen == 0) break;
            }
            if (!matched) {
                if (i < slen)
                    sb_appendc(&sb, s[i]);
                i++;
            }
            if (!global && replaced) {
                /* Append rest of string */
                sb_append(&sb, s + i);
                break;
            }
        }
        free(tmp);
        char *r = arena_strdup(sh->scratch, sb_str(&sb));
        sb_free(&sb);
        return r;
    }

    /* Fallback: return raw value */
    return braced_ret(sh, val);
}

/* -------------------------------------------------------------------------
 * Command substitution: run command, capture stdout
 * ------------------------------------------------------------------------- */

static char *cmd_subst(shell_ctx_t *sh, const char *cmd)
{
    /* Create a pipe */
    int pipefd[2];
    fflush(NULL);   /* flush all buffers before fork so child doesn't inherit pending output */
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return arena_strdup(sh->scratch, "");
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return arena_strdup(sh->scratch, "");
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Run the command in a sub-shell */
        shell_ctx_t sub;
        shell_init(&sub, 0, NULL);
        sub.vars         = sh->vars;    /* inherit vars */
        sub.positional   = sh->positional;
        sub.positional_n = sh->positional_n;
        sub.script_name  = sh->script_name;
        sub.last_exit    = sh->last_exit;  /* inherit $? */
        /* Inherit shell options: a command substitution runs in a subshell of
         * the current environment, so `set -f`, `set -u`, `set -C`, ... are in
         * effect inside it. shell_init() cleared them; copy the parent's. Without
         * this, `saved=$(set +o)` reported every option off regardless of the
         * real state, breaking modernish's option save/restore (_IN/opt). */
        sub.opt_a = sh->opt_a;
        sub.opt_e = sh->opt_e;
        sub.opt_u = sh->opt_u;
        sub.opt_x = sh->opt_x;
        sub.opt_f = sh->opt_f;
        sub.opt_n = sh->opt_n;
        sub.opt_m = sh->opt_m;
        sub.opt_C = sh->opt_C;
        sub.opt_pipefail = sh->opt_pipefail;
        sub.shell_pid    = sh->shell_pid;  /* $$ is the main shell's PID, not the
                                            * command-substitution child's */
        memcpy(sub.funcs, sh->funcs, sizeof(sh->funcs)); /* inherit functions */
        /* Inherit aliases too: POSIX expands aliases inside `$(...)` (dash does),
         * and modernish's whole DSL is alias-based -- without this, a nested
         * `LOOP`/`DO`/`DONE` (or any alias) used inside a command substitution
         * was "command not found". Safe across fork: the child only reads these
         * pointers and _exit()s. */
        memcpy(sub.aliases, sh->aliases, sizeof(sh->aliases));
        /* Clear set_in_this_shell for inherited traps. NOTE: a command
         * substitution does NOT inherit the parent's signal trap actions -- dash
         * resets them to default in the `$(...)` subshell, and matching that is
         * what modernish expects (a PIPE trap does not fire for a SIGPIPE taken
         * by a pipeline stage inside a command substitution). */
        for (int i = 0; i < NSIG; i++)
            sub.traps[i].set_in_this_shell = 0;
        shell_run_string(&sub, cmd);
        int ex = sub.last_exit;
        /* Fire EXIT trap if set in this command substitution */
        const char *exit_act = sub.traps[0].action;
        if (sub.traps[0].set_in_this_shell &&
            exit_act != SHELL_TRAP_DEFAULT && exit_act[0] != '\0') {
            sub.traps[0].action = SHELL_TRAP_DEFAULT;
            shell_run_string(&sub, exit_act);
        }
        shell_free(&sub);
        fflush(NULL);
        _exit(ex);
    }

    /* Parent: read from pipe */
    close(pipefd[1]);

    strbuf_t sb;
    sb_init(&sb, 256);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        sb_appendn(&sb, buf, (size_t)n);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    /* Record the substitution's status, but do NOT touch sh->last_exit.
     *
     * $? must not be disturbed by a command substitution inside an ordinary
     * word: `echo $(false); echo $?` prints 0, because that is echo's status.
     *
     * But POSIX 2.9.1 says a command with NO command name that contains a
     * command substitution completes with the status of the last substitution
     * performed -- so `v=$(false); echo $?` must print 1. exec_simple_cmd's
     * assignment-only path reads this.
     *
     * Nothing recorded the status at all before, so cmdsub_exit in exec.c was
     * always 0 and `v=$(cmd); ret=$?` always saw success. That is the single
     * most common idiom in a configure script, and it made them silently take
     * the success branch of every probe: zlib's ./configure concluded it was
     * building for IBM s390x on x86_64, emitted -DHAVE_S390X_VX -mzarch, and
     * exited 0 with a Makefile that could not build. */
    sh->last_cmdsub_exit = WIFEXITED(status) ? WEXITSTATUS(status)
                         : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                         : 1;

    /* Strip trailing newlines (POSIX) */
    while (sb.len > 0 && sb.buf[sb.len - 1] == '\n') {
        sb.len--;
        sb.buf[sb.len] = '\0';
    }

    char *result = arena_strdup(sh->scratch, sb_str(&sb));
    sb_free(&sb);
    return result;
}

/* -------------------------------------------------------------------------
 * Arithmetic evaluation (recursive descent)
 * ------------------------------------------------------------------------- */

typedef struct {
    const char    *src;
    size_t         pos;
    shell_ctx_t   *sh;
    int            depth;   /* recursion guard for operands expanded from params */
} arith_ctx_t;

static void arith_skip_ws(arith_ctx_t *ac)
{
    while (ac->src[ac->pos] == ' ' || ac->src[ac->pos] == '\t')
        ac->pos++;
}

static long arith_expr(arith_ctx_t *ac);
static long arith_ternary(arith_ctx_t *ac);

/* Read one variable operand into `buf`, substituting any embedded ${...} or
 * $name parameter expansion by its VALUE (POSIX 2.6.4: the arithmetic
 * expression is parameter-expanded first, then evaluated). A run of literal
 * name characters is copied as-is; a $-reference is replaced by its value. So:
 *   ${V}   (V=foo)                 -> operand "foo"   (then evaluated as var foo)
 *   $x     (x=5)                   -> operand "5"     (numeric literal)
 *   _Msh__V${_Msh_push_V}__SP      -> one built name  (modernish push/pop)
 * Without this, arith resolved `${V}` by atol(V's value), so `${V}` with a
 * non-numeric value was 0 and a name could not be built from an expansion. */
static void arith_read_operand(arith_ctx_t *ac, char *buf, size_t bufsz)
{
    size_t ni = 0;
    for (;;) {
        char c = ac->src[ac->pos];
        if (c == '$' && ac->src[ac->pos + 1] == '{') {
            /* Full ${...} parameter expansion: `${x-default}`, `${x:-y}`,
             * `${#x}`, ... all go through the normal machinery. A verbatim var
             * lookup of the brace body treated `${2-1}` (positional 2, default
             * 1) as a variable literally named "2-1" and, under `set -u`, aborted
             * with "2-1: unbound variable" -- which broke modernish's inc/dec. */
            char exptext[512];
            size_t ei = 0;
            exptext[ei++] = '$';
            exptext[ei++] = '{';
            ac->pos += 2;                 /* skip "${" */
            int bd = 1;
            while (ac->src[ac->pos] && bd > 0 && ei < sizeof(exptext) - 1) {
                char cc = ac->src[ac->pos];
                if (cc == '{') bd++;
                else if (cc == '}') bd--;
                exptext[ei++] = cc;
                ac->pos++;
                if (bd == 0) break;
            }
            exptext[ei] = '\0';
            char *val = expand_word(ac->sh, exptext);
            if (val)
                while (*val && ni < bufsz - 1)
                    buf[ni++] = *val++;
            continue;
        }
        if (c == '$') {
            ac->pos++;
            char inner[256];
            size_t ii = 0;
            if (is_special_var(ac->src[ac->pos]) ||
                       (ac->src[ac->pos] >= '1' && ac->src[ac->pos] <= '9')) {
                inner[ii++] = ac->src[ac->pos++];
            } else {
                while (is_name_char((unsigned char)ac->src[ac->pos]) &&
                       ii < sizeof(inner) - 1)
                    inner[ii++] = ac->src[ac->pos++];
            }
            inner[ii] = '\0';
            const char *v = sh_getvar(ac->sh, inner);
            if (!v && ac->sh->opt_u) {
                fprintf(stderr, "silex: %s: unbound variable\n", inner);
                exit(1);
            }
            if (v)
                while (*v && ni < bufsz - 1)
                    buf[ni++] = *v++;
        } else if (is_name_char((unsigned char)c)) {
            if (ni < bufsz - 1)
                buf[ni++] = c;
            ac->pos++;
        } else {
            break;
        }
    }
    buf[ni] = '\0';
}

static long arith_primary(arith_ctx_t *ac)
{
    arith_skip_ws(ac);
    char c = ac->src[ac->pos];

    /* Parenthesised expression */
    if (c == '(') {
        ac->pos++;
        long v = arith_expr(ac);
        arith_skip_ws(ac);
        if (ac->src[ac->pos] == ')') ac->pos++;
        return v;
    }

    /* Pre-increment: ++var */
    if (c == '+' && ac->src[ac->pos + 1] == '+') {
        ac->pos += 2;
        arith_skip_ws(ac);
        char preinc_name[256]; size_t preinc_ni = 0;
        while (is_name_char((unsigned char)ac->src[ac->pos]) && preinc_ni < sizeof(preinc_name)-1)
            preinc_name[preinc_ni++] = ac->src[ac->pos++];
        preinc_name[preinc_ni] = '\0';
        const char *preinc_v = sh_getvar(ac->sh, preinc_name);
        long preinc_nv = (preinc_v ? atol(preinc_v) : 0L) + 1;
        char preinc_nb[32]; snprintf(preinc_nb, sizeof(preinc_nb), "%ld", preinc_nv);
        vars_set(&ac->sh->vars, preinc_name, preinc_nb);
        return preinc_nv;
    }

    /* Pre-decrement: --var */
    if (c == '-' && ac->src[ac->pos + 1] == '-') {
        ac->pos += 2;
        arith_skip_ws(ac);
        char predec_name[256]; size_t predec_ni = 0;
        while (is_name_char((unsigned char)ac->src[ac->pos]) && predec_ni < sizeof(predec_name)-1)
            predec_name[predec_ni++] = ac->src[ac->pos++];
        predec_name[predec_ni] = '\0';
        const char *predec_v = sh_getvar(ac->sh, predec_name);
        long predec_nv = (predec_v ? atol(predec_v) : 0L) - 1;
        char predec_nb[32]; snprintf(predec_nb, sizeof(predec_nb), "%ld", predec_nv);
        vars_set(&ac->sh->vars, predec_name, predec_nb);
        return predec_nv;
    }

    /* Unary minus */
    if (c == '-') {
        ac->pos++;
        return -arith_primary(ac);
    }

    /* Unary plus */
    if (c == '+') {
        ac->pos++;
        return arith_primary(ac);
    }

    /* Unary NOT */
    if (c == '!') {
        ac->pos++;
        return !arith_primary(ac);
    }

    /* Bitwise NOT */
    if (c == '~') {
        ac->pos++;
        return ~arith_primary(ac);
    }

    /* $(cmd) command substitution inside arithmetic */
    if (c == '$' && ac->src[ac->pos + 1] == '(') {
        ac->pos += 2;  /* skip "$(" */
        const char *start = ac->src + ac->pos;
        int d = 1;
        while (ac->src[ac->pos] && d > 0) {
            if (ac->src[ac->pos] == '(') d++;
            else if (ac->src[ac->pos] == ')') d--;
            if (d > 0) ac->pos++;
            else ac->pos++;
        }
        size_t clen = (size_t)(ac->src + ac->pos - 1 - start);
        char *cmd = strndup(start, clen);
        char *result = cmd_subst(ac->sh, cmd ? cmd : "");
        free(cmd);
        return result ? strtol(result, NULL, 10) : 0L;
    }

    /* A variable operand: name characters and/or ${...}/$name expansions
     * (read_operand substitutes each expansion by its value). The built token
     * is a numeric literal (e.g. `$x` with x=5 -> "5") or a variable that may
     * be an lvalue for ++/--/assignment. Special parameters ($#, $?, ...) are
     * handled inside read_operand. */
    if (c == '$' || is_alpha_underscore((unsigned char)c)) {
        char namebuf[256];
        arith_read_operand(ac, namebuf, sizeof namebuf);

        /* A fully-numeric operand (from an expansion or a bare literal-looking
         * name) is a constant, not a variable to look up or assign. */
        char *endp;
        long litval  = strtol(namebuf, &endp, 0);
        int  is_num  = (namebuf[0] != '\0' && *endp == '\0');

        /* If the operand is neither a number nor a plain variable name, it was
         * produced by expanding a parameter whose VALUE is itself an arithmetic
         * expression — e.g. a positional `$1` holding "i = (1)", or `v="2 + 3"`
         * used as `($v)`. POSIX 2.6.4 says the expression is parameter-expanded
         * and THEN evaluated, so the substituted text must be parsed as a
         * sub-expression (arith_read_operand only substituted the value; it did
         * not evaluate it). Recurse, with a depth guard against self-referential
         * values like v='$v'. Assignments inside it take effect (needed so a
         * later operand in the same expression can read the assigned var). */
        if (!is_num && namebuf[0]) {
            int is_name = is_alpha_underscore((unsigned char)namebuf[0]);
            for (const char *q = namebuf + 1; is_name && *q; q++)
                if (!is_name_char((unsigned char)*q)) is_name = 0;
            if (!is_name) {
                if (ac->depth >= 64)
                    return 0;   /* runaway expansion: give up rather than loop */
                arith_ctx_t sub;
                sub.src   = namebuf;
                sub.pos   = 0;
                sub.sh    = ac->sh;
                sub.depth = ac->depth + 1;
                return arith_expr(&sub);
            }
        }

        const char *v = is_num ? NULL : sh_getvar(ac->sh, namebuf);
        long cur_val = is_num ? litval : (v ? atol(v) : 0L);

        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;

        if (!is_num && p[0] == '+' && p[1] == '+') {       /* post-increment */
            ac->pos += 2;
            char nb[32]; snprintf(nb, sizeof nb, "%ld", cur_val + 1);
            vars_set(&ac->sh->vars, namebuf, nb);
            return cur_val;
        }
        if (!is_num && p[0] == '-' && p[1] == '-') {       /* post-decrement */
            ac->pos += 2;
            char nb[32]; snprintf(nb, sizeof nb, "%ld", cur_val - 1);
            vars_set(&ac->sh->vars, namebuf, nb);
            return cur_val;
        }

        int assign_op = 0;
        size_t oplen  = 0;
        if      (p[0]=='<' && p[1]=='<' && p[2]=='=') { assign_op=6;  oplen=3; }
        else if (p[0]=='>' && p[1]=='>' && p[2]=='=') { assign_op=7;  oplen=3; }
        else if (p[0]=='+' && p[1]=='=')              { assign_op=1;  oplen=2; }
        else if (p[0]=='-' && p[1]=='=')              { assign_op=2;  oplen=2; }
        else if (p[0]=='*' && p[1]=='=')              { assign_op=3;  oplen=2; }
        else if (p[0]=='/' && p[1]=='=')              { assign_op=4;  oplen=2; }
        else if (p[0]=='%' && p[1]=='=')              { assign_op=5;  oplen=2; }
        else if (p[0]=='&' && p[1]=='=')              { assign_op=8;  oplen=2; }
        else if (p[0]=='|' && p[1]=='=')              { assign_op=9;  oplen=2; }
        else if (p[0]=='^' && p[1]=='=')              { assign_op=10; oplen=2; }
        else if (p[0]=='=' && p[1]!='=')              { assign_op=11; oplen=1; }

        if (assign_op && !is_num) {
            ac->pos += oplen;
            long rhs = arith_ternary(ac);  /* comma is lower precedence than assign */
            long result;
            switch (assign_op) {
            case 1:  result = cur_val + rhs; break;
            case 2:  result = cur_val - rhs; break;
            case 3:  result = cur_val * rhs; break;
            case 4:
                if (!rhs) { fprintf(stderr, "silex: sh: arithmetic expression: division by zero\n"); exit(2); }
                result = cur_val / rhs; break;
            case 5:
                if (!rhs) { fprintf(stderr, "silex: sh: arithmetic expression: division by zero\n"); exit(2); }
                result = cur_val % rhs; break;
            case 6:  result = cur_val << rhs; break;
            case 7:  result = cur_val >> rhs; break;
            case 8:  result = cur_val & rhs; break;
            case 9:  result = cur_val | rhs; break;
            case 10: result = cur_val ^ rhs; break;
            case 11: result = rhs; break;
            default: result = cur_val;
            }
            char numbuf[32];
            snprintf(numbuf, sizeof(numbuf), "%ld", result);
            vars_set(&ac->sh->vars, namebuf, numbuf);
            return result;
        }

        return cur_val;
    }

    /* Integer literal (decimal, hex, octal) */
    if (is_digit((unsigned char)c)) {
        char *end;
        long v = strtol(ac->src + ac->pos, &end, 0);
        ac->pos = (size_t)(end - ac->src);
        return v;
    }

    return 0L;
}

static long arith_mul(arith_ctx_t *ac)
{
    long left = arith_primary(ac);
    for (;;) {
        arith_skip_ws(ac);
        char op = ac->src[ac->pos];
        if (op != '*' && op != '/' && op != '%') break;
        ac->pos++;
        long right = arith_primary(ac);
        if (op == '*') left *= right;
        else if (op == '/') {
            if (!right) {
                fprintf(stderr, "silex: sh: arithmetic expression: division by zero\n");
                exit(2);
            }
            left /= right;
        } else {
            if (!right) {
                fprintf(stderr, "silex: sh: arithmetic expression: division by zero\n");
                exit(2);
            }
            left %= right;
        }
    }
    return left;
}

static long arith_add(arith_ctx_t *ac)
{
    long left = arith_mul(ac);
    for (;;) {
        arith_skip_ws(ac);
        char op = ac->src[ac->pos];
        if (op != '+' && op != '-') break;
        /* Do not consume a -- or ++ by accident; stop at two consecutive */
        if (op == '-' && ac->src[ac->pos + 1] == '-') break;
        if (op == '+' && ac->src[ac->pos + 1] == '+') break;
        /* Do not consume += or -= (compound assignment) */
        if (ac->src[ac->pos + 1] == '=') break;
        ac->pos++;
        long right = arith_mul(ac);
        if (op == '+') left += right;
        else left -= right;
    }
    return left;
}

static long arith_shift(arith_ctx_t *ac)
{
    long left = arith_add(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        if (p[0] == '<' && p[1] == '<' && p[2] != '=') {
            ac->pos += 2;
            long right = arith_add(ac);
            /* Left-shifting a SIGNED value into or past the sign bit is
             * undefined behaviour. Do the shift on the unsigned representation
             * and convert back -- that is well-defined and gives the two's
             * complement result every shell expects. */
            left = (right >= 0 && right < 64)
                 ? (long)((unsigned long)left << right)
                 : 0;
        } else if (p[0] == '>' && p[1] == '>' && p[2] != '=') {
            ac->pos += 2;
            long right = arith_add(ac);
            /* Right shift of a negative value is implementation-defined, not
             * undefined; every compiler we target makes it arithmetic, which is
             * the behaviour POSIX shells have. Keep it signed on purpose. */
            /* cppcheck-suppress shiftTooManyBitsSigned */
            left = (right >= 0 && right < 64) ? (left >> right) : 0;
        } else {
            break;
        }
    }
    return left;
}

static long arith_cmp(arith_ctx_t *ac)
{
    long left = arith_shift(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        int op = 0;
        size_t oplen = 1;
        if      (p[0]=='<' && p[1]=='=')              { op=1; oplen=2; }
        else if (p[0]=='>' && p[1]=='=')              { op=2; oplen=2; }
        else if (p[0]=='<' && p[1]!='<' && p[1]!='=') { op=3; oplen=1; }
        else if (p[0]=='>' && p[1]!='>' && p[1]!='=') { op=4; oplen=1; }
        else if (p[0]=='=' && p[1]=='=')              { op=5; oplen=2; }
        else if (p[0]=='!' && p[1]=='=')              { op=6; oplen=2; }
        else break;
        ac->pos += oplen;
        long right = arith_shift(ac);
        switch (op) {
        case 1: left = left <= right; break;
        case 2: left = left >= right; break;
        case 3: left = left < right;  break;
        case 4: left = left > right;  break;
        case 5: left = left == right; break;
        case 6: left = left != right; break;
        }
    }
    return left;
}

static long arith_bitand(arith_ctx_t *ac)
{
    long left = arith_cmp(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        if (p[0] == '&' && p[1] != '&' && p[1] != '=') {
            ac->pos++;
            left &= arith_cmp(ac);
        } else {
            break;
        }
    }
    return left;
}

static long arith_bitxor(arith_ctx_t *ac)
{
    long left = arith_bitand(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        if (p[0] == '^' && p[1] != '=') {
            ac->pos++;
            left ^= arith_bitand(ac);
        } else {
            break;
        }
    }
    return left;
}

static long arith_bitor(arith_ctx_t *ac)
{
    long left = arith_bitxor(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        if (p[0] == '|' && p[1] != '|' && p[1] != '=') {
            ac->pos++;
            left |= arith_bitxor(ac);
        } else {
            break;
        }
    }
    return left;
}

static long arith_logical_and(arith_ctx_t *ac)
{
    long left = arith_bitor(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        if (p[0] == '&' && p[1] == '&') {
            ac->pos += 2;
            long right = arith_bitor(ac);
            left = (left && right) ? 1 : 0;
        } else {
            break;
        }
    }
    return left;
}

static long arith_logical_or(arith_ctx_t *ac)
{
    long left = arith_logical_and(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        if (p[0] == '|' && p[1] == '|') {
            ac->pos += 2;
            long right = arith_logical_and(ac);
            left = (left || right) ? 1 : 0;
        } else {
            break;
        }
    }
    return left;
}

static long arith_ternary(arith_ctx_t *ac)
{
    long cond = arith_logical_or(ac);
    arith_skip_ws(ac);
    if (ac->src[ac->pos] != '?') return cond;
    ac->pos++;  /* consume '?' */
    long t = arith_ternary(ac);  /* true branch (right-associative) */
    arith_skip_ws(ac);
    if (ac->src[ac->pos] == ':') ac->pos++;  /* consume ':' */
    long f = arith_ternary(ac);  /* false branch */
    return cond ? t : f;
}

static long arith_expr(arith_ctx_t *ac)
{
    long val = arith_ternary(ac);
    arith_skip_ws(ac);
    while (ac->src[ac->pos] == ',') {
        ac->pos++;
        val = arith_ternary(ac);
        arith_skip_ws(ac);
    }
    return val;
}

long expand_arith(shell_ctx_t *sh, const char *expr)
{
    arith_ctx_t ac;
    ac.src   = expr;
    ac.pos   = 0;
    ac.sh    = sh;
    ac.depth = 0;
    return arith_expr(&ac);
}

/* Advance `p` (pointing at the first char after the '(' of a `$(`) to the
 * matching ')', mirroring the lexer's scan_cmd_subst: single/double quotes,
 * backslash escapes, `#` comments, and nested parens are all skipped so a stray
 * `)` inside any of them does not end the substitution early. Returns a pointer
 * to the closing ')' (or to the terminating NUL when unterminated). The word was
 * tokenised with exactly these rules, so the body this delimits matches. */
static const char *cmdsubst_body_end(const char *p)
{
    int depth = 1;
    int prev  = '(';
    while (*p && depth > 0) {
        char c = *p;
        if (c == '#' &&
            (prev == ' ' || prev == '\t' || prev == '\n' || prev == '(' ||
             prev == ';' || prev == '&'  || prev == '|')) {
            p++;
            while (*p && *p != '\n') p++;
            prev = '#';
            continue;
        }
        prev = c;
        if (c == '\\') {
            p++;
            if (*p) p++;
            continue;
        }
        if (c == '\'') {
            p++;
            while (*p && *p != '\'') p++;
            if (*p) p++;
            continue;
        }
        if (c == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p) p++;
            continue;
        }
        if (c == '(') { depth++; p++; }
        else if (c == ')') { depth--; if (depth == 0) break; p++; }
        else p++;
    }
    return p;
}

/* -------------------------------------------------------------------------
 * skip_dquote_end: advance p from first char after opening '"' to just past
 * the matching '"'.  Properly handles $(...), ${...}, $((...)), backticks.
 * ------------------------------------------------------------------------- */
static const char *skip_dquote_end(const char *p)
{
    while (*p) {
        if (*p == '"')
            return p + 1;
        if (*p == '\\') {
            if (p[1]) p += 2; else p++;
            continue;
        }
        if (*p == '$') {
            p++;
            if (*p == '(' && p[1] == '(') {
                /* $(( )) arithmetic */
                p += 2;
                int d = 2;
                while (*p && d > 0) {
                    if (*p == '(') d++;
                    else if (*p == ')') d--;
                    p++;
                }
            } else if (*p == '(') {
                /* $( ) cmd substitution — quote/comment/backslash-aware. */
                p = cmdsubst_body_end(p + 1);
                if (*p == ')') p++;
            } else if (*p == '{') {
                p++;
                int d = 1;
                while (*p && d > 0) {
                    if (*p == '{') d++;
                    else if (*p == '}') d--;
                    p++;
                }
            }
            continue;
        }
        if (*p == '`') {
            p++;
            while (*p && *p != '`') {
                if (*p == '\\') { if (p[1]) p += 2; else p++; continue; }
                p++;
            }
            if (*p == '`') p++;
            continue;
        }
        p++;
    }
    return p;
}

/* -------------------------------------------------------------------------
 * Core expand_into: walk 'word', handle quoting / substitutions,
 * append expanded text to 'out'.
 * in_dquote: 1 when inside "..."
 * ------------------------------------------------------------------------- */

/* In-band markers bracketing a quoted (non-splittable) region of a word that
 * will otherwise undergo IFS field splitting. See shell.h (emit_guards). */
#define QG_OPEN  '\x02'
#define QG_CLOSE '\x03'

/* "$@"/"$*" is flattened into one string with 0x01 field-boundary markers that a
 * later pass splits on. A LITERAL 0x01 (or a quote-guard 0x02/0x03) inside a
 * positional parameter would be mistaken for a marker -- modernish injects such
 * control bytes on purpose. So when a word emits real boundaries, each
 * parameter's data is appended through pp_escape_append(), which encodes every
 * reserved byte (0x01-0x04) as PP_ESC followed by the byte with bit 6 set;
 * pp_decode() reverses it after splitting, restoring the literal byte. PP_ESC
 * (0x04) itself is escaped so decoding is unambiguous. */
#define PP_ESC 0x04
static void pp_escape_append(strbuf_t *out, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == 0x01 || c == 0x02 || c == 0x03 || c == PP_ESC) {
            sb_appendc(out, (char)PP_ESC);
            sb_appendc(out, (char)(c | 0x40u));
        } else {
            sb_appendc(out, (char)c);
        }
    }
}

static char *pp_decode(arena_t *a, const char *s)
{
    size_t n = strlen(s);
    char *out = arena_alloc(a, n + 1);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] == PP_ESC && i + 1 < n) {
            out[w++] = (char)((unsigned char)s[i + 1] & ~0x40u);
            i++;
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = '\0';
    return out;
}

/* Append DATA (a variable/command-substitution/positional value, or a literal
 * word character) to a word being built for field splitting. When this word
 * will be post-processed (emit_guards -> it carries 0x01 boundaries and/or
 * 0x02/0x03 quote guards), reserved bytes in the data are encoded so a literal
 * copy is not mistaken for a marker and stripped; the caller decodes each field
 * afterwards. When the word is not post-processed, data passes through verbatim. */
static void emit_data(shell_ctx_t *sh, strbuf_t *out, const char *s)
{
    if (sh->emit_guards) pp_escape_append(out, s);
    else                 sb_append(out, s);
}

/* Open/close a quoted region. Only the outermost open/close emit a marker
 * (depth coalesces nested quotes), and only when this word will be split. */
static void guard_open(shell_ctx_t *sh, strbuf_t *out)
{
    if (!sh->emit_guards) return;
    if (sh->quote_guard_depth++ == 0) {
        sb_appendc(out, QG_OPEN);
        sh->at_quote_guard = 1;
    }
}

static void guard_close(shell_ctx_t *sh, strbuf_t *out)
{
    if (!sh->emit_guards) return;
    if (sh->quote_guard_depth > 0 && --sh->quote_guard_depth == 0)
        sb_appendc(out, QG_CLOSE);
}

static void expand_into(shell_ctx_t *sh, const char *word, strbuf_t *out,
                        int in_dquote)
{
    const char *p = word;

    while (*p) {
        /* Single quotes: literal (only outside double-quotes) */
        if (*p == '\'' && !in_dquote) {
            p++; /* skip opening ' */
            guard_open(sh, out);
            while (*p && *p != '\'')
                sb_appendc(out, *p++);
            guard_close(sh, out);
            if (*p == '\'') p++;
            continue;
        }

        /* Double quotes */
        if (*p == '"' && !in_dquote) {
            p++; /* skip opening " */
            guard_open(sh, out);
            expand_into(sh, p, out, 1);
            guard_close(sh, out);
            /* Advance p past the double-quoted content (skip_dquote_end handles
             * nested $(...), ${...}, $((...)). expand_into already output the
             * content; we just need to advance the outer pointer. */
            p = skip_dquote_end(p);
            continue;
        }

        /* A `"` inside the WORD of a double-quoted `"${v-WORD}"` is redundant:
         * the WORD already sits in the double-quoted context, so the quote is
         * removed and the scan continues (staying quoted) rather than closing a
         * section. This keeps `"${v-"a${nl}b"}"` from truncating at the inner
         * quote. (Escaped `\"` is handled by the backslash branch above, so a
         * literal `"` in the value is unaffected.) */
        if (*p == '"' && in_dquote && sh->pp_word_dq) {
            p++;
            continue;
        }

        /* End of double-quoted section. In a here-document body (expanded with
         * in_dquote=1 so expansions run and `'` stays literal) a `"` is ordinary
         * text, not a closing delimiter, so fall through to emit it literally. */
        if (*p == '"' && in_dquote && !sh->in_heredoc) {
            p++;
            return;
        }

        /* Backslash escaping */
        if (*p == '\\') {
            p++;
            if (!*p) break;
            if (in_dquote) {
                /* Inside "...", backslash only escapes $, `, ", \, newline */
                if (*p == '$' || *p == '`' || *p == '"' ||
                    *p == '\\' || *p == '\n') {
                    if (*p != '\n') sb_appendc(out, *p);
                } else {
                    sb_appendc(out, '\\');
                    sb_appendc(out, *p);
                }
            } else {
                if (*p == '\n') { p++; continue; } /* line continuation */
                sb_appendc(out, *p);
            }
            p++;
            continue;
        }

        /* $ expansions */
        if (*p == '$') {
            p++;
            if (*p == '\0') {
                sb_appendc(out, '$');
                break;
            }

            /* $((arith)) */
            if (p[0] == '(' && p[1] == '(') {
                p += 2;
                /* Find matching )) */
                const char *start = p;
                int depth = 2;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    if (depth > 0) p++;
                    else p++;
                }
                /* p now points past last ')' */
                size_t alen = (size_t)((p - 2) - start); /* trim )) */
                /* Actually: find closing )) by scanning */
                char *arith_src = strndup(start, alen);
                long arith_val = expand_arith(sh, arith_src ? arith_src : "");
                free(arith_src);
                char abuf[32];
                snprintf(abuf, sizeof(abuf), "%ld", arith_val);
                emit_data(sh, out, abuf);
                continue;
            }

            /* $(cmd) */
            if (*p == '(') {
                p++;
                const char *start = p;
                /* Quote/comment/backslash-aware scan for the matching ')': a
                 * naive paren count stopped at a ')' inside a comment or quote,
                 * so `$( # ) )` or `x=$(case $y in *) ...) esac)` extracted the
                 * wrong body and left stray text behind. */
                p = cmdsubst_body_end(p);
                size_t clen = (size_t)(p - start);
                if (*p == ')') p++;   /* step past the closing ')' */
                char *cmd = strndup(start, clen);
                char *result = cmd_subst(sh, cmd ? cmd : "");
                free(cmd);
                emit_data(sh, out, result);
                continue;
            }

            /* ${...} */
            if (*p == '{') {
                p++;
                const char *start = p;
                /* Find the matching '}' with the same quote/backslash awareness
                 * as the lexer's scan_param_expand: a `{`/`}` inside '...'/"..."
                 * is literal, and a `\}` is an escaped (literal) brace -- so
                 * `${v-ab\}cd}` closes at the LAST '}', not the escaped one, and
                 * `${x:+'{'\}}` (modernish) balances correctly. The backslash and
                 * quotes are kept in the body for expand_braced to remove. */
                int depth = 1;
                int sq = 0, inner_dq = 0;
                while (*p && depth > 0) {
                    if (sq) {
                        if (*p == '\'') sq = 0;
                        p++;
                        continue;
                    }
                    if (*p == '\\' && p[1]) { p += 2; continue; }
                    /* `'` quotes only outside double quotes -- the enclosing
                     * "..." (in_dquote) counts, matching the lexer scan. */
                    if (*p == '\'' && !in_dquote && !inner_dq) { sq = 1; p++; continue; }
                    if (*p == '"') { inner_dq = !inner_dq; p++; continue; }
                    /* Only an INNER "..." opened here suppresses the closing '}';
                     * the outer in_dquote does not. */
                    if (*p == '{' && !inner_dq) { depth++; p++; continue; }
                    if (*p == '}' && !inner_dq) { depth--; if (depth == 0) break; p++; continue; }
                    p++;
                }
                size_t blen = (size_t)(p - start);
                char *body = strndup(start, blen);
                if (*p == '}') p++;   /* step past the closing brace */
                char *val  = expand_braced(sh, body ? body : "", in_dquote);
                free(body);
                /* expand_braced() already ran expand_into() on any quoted word
                 * part, so `val` may itself contain guard/boundary markers with
                 * its data already escaped -- append it RAW, never through
                 * emit_data(), which would escape those markers and break them. */
                sb_append(out, val);
                continue;
            }

            /* $@ and $* — positional list */
            if (*p == '@' || *p == '*') {
                if (sh->in_heredoc || sh->in_assign) {
                    /* A here-doc body and an assignment RHS are both NON-split
                     * contexts, so $@ and $* join with IFS's first byte
                     * (unset => space, empty => nothing) instead of emitting
                     * \x01 field boundaries -- otherwise `var=$*` would leak the
                     * internal markers into the variable's value. */
                    const char *ifs = sh_getvar(sh, "IFS");
                    int  have_sep = (ifs == NULL) || (ifs[0] != '\0');
                    char sep      = (ifs == NULL) ? ' ' : ifs[0];
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0 && have_sep) sb_appendc(out, sep);
                        sb_append(out, sh->positional[pi]);
                    }
                    p++;
                    continue;
                }
                if (*p == '@' && in_dquote) {
                    /* "$@": each positional as a separate word; use \x01 boundary */
                    if (sh->positional_n == 0) {
                        /* POSIX 2.5.2: "$@" with no positional parameters
                         * generates ZERO fields, even though it is double-quoted
                         * -- unlike "$*", which generates one empty field.
                         *
                         * Appending nothing here left an empty word, and the
                         * empty-word path below then manufactured one empty
                         * field. So `f "$@"` passed one empty argument instead
                         * of none, and `exec cmd "$0" "$@"` -- the autosetup
                         * idiom, and sqlite's ./configure -- died with
                         * "Unexpected parameter:". Flag it so the field-splitting
                         * phase can drop the word entirely. */
                        sh->at_expanded_empty = 1;
                    }
                    /* Escape reserved bytes in the data only when real
                     * boundaries are emitted (>=2 params); the boundary splitter
                     * then decodes each field. With a single param no boundary is
                     * emitted and the value must pass through verbatim. */
                    int esc = (sh->positional_n >= 2) || sh->emit_guards;
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0) { sb_appendc(out, '\x01'); sh->at_field_boundary = 1; }
                        if (esc) pp_escape_append(out, sh->positional[pi]);
                        else     sb_append(out, sh->positional[pi]);
                    }
                } else if (*p == '*' && in_dquote) {
                    /* "$*": POSIX 2.5.2 -- join the positionals with the FIRST
                     * byte of IFS. An UNSET IFS joins with a space; an IFS that
                     * is set but EMPTY joins with nothing (not a space);
                     * otherwise the first byte of IFS. Conflating unset and
                     * empty produced "a b c" where empty IFS must yield "abc". */
                    const char *ifs = sh_getvar(sh, "IFS");
                    int  have_sep = (ifs == NULL) || (ifs[0] != '\0');
                    char sep      = (ifs == NULL) ? ' ' : ifs[0];
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0 && have_sep) sb_appendc(out, sep);
                        emit_data(sh, out, sh->positional[pi]);
                    }
                } else if (sh->pp_join_unquoted) {
                    /* Unquoted $* or $@ inside a ${var-WORD}/${var=WORD} word:
                     * the word forms one value that the caller field-splits, so
                     * join with IFS's first byte here (unset => space, empty =>
                     * nothing) rather than emitting \x01 boundaries. */
                    const char *ifs = sh_getvar(sh, "IFS");
                    int  have_sep = (ifs == NULL) || (ifs[0] != '\0');
                    char sep      = (ifs == NULL) ? ' ' : ifs[0];
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0 && have_sep) sb_appendc(out, sep);
                        emit_data(sh, out, sh->positional[pi]);
                    }
                } else {
                    /* Unquoted $* or $@: when IFS is empty, still split on \x01 */
                    const char *ifs = sh_getvar(sh, "IFS");
                    if (ifs && ifs[0] == '\0') {
                        /* Empty IFS: use \x01 to separate positionals */
                        int esc = (sh->positional_n >= 2) || sh->emit_guards;
                        for (int pi = 0; pi < sh->positional_n; pi++) {
                            if (pi > 0) { sb_appendc(out, '\x01'); sh->at_field_boundary = 1; }
                            if (esc) pp_escape_append(out, sh->positional[pi]);
                            else     sb_append(out, sh->positional[pi]);
                        }
                    } else {
                        /* Non-empty or unset IFS: join with space (default behavior) */
                        char spec[2] = { *p, '\0' };
                        const char *v = sh_getvar(sh, spec);
                        if (v) emit_data(sh, out, v);
                    }
                }
                p++;
                continue;
            }

            /* Special single-char vars */
            if (is_special_var(*p)) {
                char spec[2] = { *p, '\0' };
                const char *v = sh_getvar(sh, spec);
                if (v) emit_data(sh, out, v);
                p++;
                continue;
            }

            /* $NAME — collect identifier */
            if (is_alpha_underscore((unsigned char)*p)) {
                const char *start = p;
                while (is_name_char((unsigned char)*p))
                    p++;
                size_t nlen = (size_t)(p - start);
                const char *name = intern_cstrn(start, nlen);
                const char *v = sh_getvar(sh, name);
                if (v) emit_data(sh, out, v);
                else if (sh->opt_u) {
                    /* set -u: error on unset var. `name` is already the
                     * interned, NUL-terminated name -- the VLA copy that used
                     * to be here was redundant AND sized by input. */
                    fprintf(stderr, "silex: %s: unbound variable\n", name);
                    exit(1);
                }
                continue;
            }

            /* $1-$9 (already handled in sh_getvar via spec) */
            if (is_digit((unsigned char)*p)) {
                char spec[2] = { *p, '\0' };
                const char *v = sh_getvar(sh, spec);
                if (v) emit_data(sh, out, v);
                else if (sh->opt_u) {
                    fprintf(stderr, "silex: $%c: unbound variable\n", *p);
                    exit(1);
                }
                p++;
                continue;
            }

            /* Unknown — keep literal $ */
            sb_appendc(out, '$');
            continue;
        }

        /* Backtick command substitution */
        if (*p == '`') {
            p++;
            const char *start = p;
            while (*p && *p != '`') {
                if (*p == '\\' && p[1] != '\0') p++;
                p++;
            }
            size_t clen = (size_t)(p - start);
            char *cmd = strndup(start, clen);
            char *result = cmd_subst(sh, cmd ? cmd : "");
            free(cmd);
            emit_data(sh, out, result);
            if (*p == '`') p++;
            continue;
        }

        sb_appendc(out, *p++);
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

char *expand_word(shell_ctx_t *sh, const char *word)
{
    if (!word) return arena_strdup(sh->scratch, "");

    /* Tilde expansion first - not in assignment context */
    char *tilded = expand_tilde(sh, word, 0);
    const char *src = tilded ? tilded : word;

    strbuf_t sb;
    sb_init(&sb, 128);
    expand_into(sh, src, &sb, 0);
    free(tilded);

    char *result = arena_strdup(sh->scratch, sb_str(&sb));
    sb_free(&sb);
    return result;
}

/* Expand a here-document body. Parameter, command, and arithmetic expansion
 * happen (as in a double-quoted string), but `'` and `"` are literal and there
 * is no tilde expansion or field splitting. Driven by sh->in_heredoc; see the
 * flag's definition in shell.h. */
char *expand_word_heredoc(shell_ctx_t *sh, const char *word)
{
    if (!word) return arena_strdup(sh->scratch, "");
    strbuf_t sb;
    sb_init(&sb, 128);
    int saved = sh->in_heredoc;
    sh->in_heredoc = 1;
    expand_into(sh, word, &sb, 1);   /* in_dquote=1: expand, but quotes literal */
    sh->in_heredoc = saved;
    char *result = arena_strdup(sh->scratch, sb_str(&sb));
    sb_free(&sb);
    return result;
}

/* Advance past the construct starting at *p, if p is at one.
 *
 * Used to find the colons in an assignment RHS that are genuinely PATH-style
 * delimiters, as opposed to colons that merely happen to sit inside a
 * quotation or an expansion. Returns a pointer past the construct, or p itself
 * if there is no construct here.
 *
 * Handles: \x  '...'  "..."  ${...}  $(...)  `...`  -- with nesting, so
 * ${a:-${b:-c}} and $(f $(g)) are skipped whole.
 */
static const char *skip_construct(const char *p)
{
    if (*p == '\\' && p[1])
        return p + 2;

    if (*p == '\'') {
        const char *q = p + 1;
        while (*q && *q != '\'')
            q++;
        return *q ? q + 1 : q;              /* unterminated: stop at NUL */
    }

    if (*p == '"') {
        const char *q = p + 1;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1])
                q += 2;
            else if (*q == '$' && (q[1] == '{' || q[1] == '('))
                q = skip_construct(q);      /* colons inside are not delimiters */
            else if (*q == '`')
                q = skip_construct(q);
            else
                q++;
        }
        return *q ? q + 1 : q;
    }

    if (*p == '`') {
        const char *q = p + 1;
        while (*q && *q != '`') {
            if (*q == '\\' && q[1]) q += 2;
            else                    q++;
        }
        return *q ? q + 1 : q;
    }

    if (*p == '$' && p[1] == '{') {
        const char *q = p + 2;
        int depth = 1;
        while (*q && depth > 0) {
            if (*q == '\\' && q[1])                       { q += 2; continue; }
            if (*q == '\'' || *q == '"' || *q == '`')      { q = skip_construct(q); continue; }
            if (*q == '$' && (q[1] == '{' || q[1] == '(')) { q = skip_construct(q); continue; }
            if (*q == '{') depth++;
            if (*q == '}') depth--;
            q++;
        }
        return q;
    }

    if (*p == '$' && p[1] == '(') {
        const char *q = p + 2;
        int depth = 1;
        while (*q && depth > 0) {
            if (*q == '\\' && q[1])                       { q += 2; continue; }
            if (*q == '\'' || *q == '"' || *q == '`')      { q = skip_construct(q); continue; }
            if (*q == '$' && (q[1] == '{' || q[1] == '(')) { q = skip_construct(q); continue; }
            if (*q == '(') depth++;
            if (*q == ')') depth--;
            q++;
        }
        return q;
    }

    return p;
}

/* Append `c` to `out` so fnmatch() treats it as a literal, not a metacharacter. */
static void pat_emit_literal(strbuf_t *out, char c)
{
    if (c == '\\' || c == '*' || c == '?' || c == '[' || c == ']')
        sb_appendc(out, '\\');
    sb_appendc(out, c);
}

/* Expand `word` into an fnmatch()-ready pattern, preserving POSIX quoting.
 *
 * expand_word() quote-removes and returns a flat string, losing which
 * characters were quoted -- so `case a in "*")` wrongly matched (the quoted '*'
 * reached fnmatch as an active wildcard) and modernish's FTL_SQBKSL check (a
 * quoted backslash in a case pattern) failed, aborting init.
 *
 * POSIX 2.13/2.14: a pattern undergoes tilde/parameter/command/arithmetic
 * expansion, then characters that were QUOTED -- via '...', "...", or a
 * backslash, INCLUDING the results of expansions performed inside double quotes
 * -- are literal, while unquoted characters and the results of UNQUOTED
 * expansions remain active pattern metacharacters. So `case a in "*")` must not
 * match, but `p='*'; case a in $p)` must.
 *
 * Strategy: walk the source in spans. Quoted spans (single/double/backslash)
 * are expanded (where applicable) and their bytes emitted escaped-if-meta;
 * unquoted runs -- with ${...}, $(...), `...` skipped whole so their inner
 * quotes are not mistaken for span boundaries -- are expanded by expand_into()
 * and emitted as-is, keeping their metacharacters active.
 *
 * Known limitation: a quoted metacharacter inside an UNQUOTED expansion word,
 * e.g. `${x-"*"}`, is not yet made literal (it needs the same quote tracking
 * inside expand_braced that ${x+"$@"} field-splitting does). Not reached by any
 * common construct; revisit if a modernish check needs it.
 */
char *expand_word_pattern(shell_ctx_t *sh, const char *word)
{
    if (!word) return arena_strdup(sh->scratch, "");

    /* Fast path: a pattern with no quoting or expansion is already an
     * fnmatch-ready pattern -- its metacharacters are all active and there is
     * nothing to escape. This is the overwhelmingly common case (`a*`, `*.c`,
     * `foo`), and skipping the walk keeps `case` in a hot loop as cheap as the
     * old quote-removing path. */
    {
        int plain = 1;
        for (const char *q = word; *q; q++) {
            if (*q == '\'' || *q == '"' || *q == '\\' ||
                *q == '$'  || *q == '`' || *q == '~') { plain = 0; break; }
        }
        if (plain)
            return arena_strdup(sh->scratch, word);
    }

    char *tilded    = expand_tilde(sh, word, 0);
    const char *src = tilded ? tilded : word;

    strbuf_t out;
    sb_init(&out, 128);

    const char *p = src;
    while (*p) {
        if (*p == '\'') {
            /* Single-quoted: literal, no expansion. */
            p++;
            while (*p && *p != '\'')
                pat_emit_literal(&out, *p++);
            if (*p == '\'') p++;
            continue;
        }
        if (*p == '"') {
            /* Double-quoted: expansions happen, but the span is quoted, so the
             * whole result is literal. Expand into a temp, then escape. */
            p++;
            strbuf_t tmp;
            sb_init(&tmp, 64);
            expand_into(sh, p, &tmp, 1);
            for (const char *q = sb_str(&tmp); *q; q++)
                pat_emit_literal(&out, *q);
            sb_free(&tmp);
            p = skip_dquote_end(p);
            continue;
        }
        if (*p == '\\') {
            /* Backslash quotes the next character -> literal. */
            p++;
            if (!*p) { sb_appendc(&out, '\\'); break; }
            pat_emit_literal(&out, *p++);
            continue;
        }
        /* Unquoted run: up to the next top-level ' " \, with ${...}/$(...)/`...`
         * skipped whole so their internal quotes don't end the run. Expanded
         * as-is, keeping metacharacters (literal or expansion-derived) active. */
        const char *run = p;
        while (*p && *p != '\'' && *p != '"' && *p != '\\') {
            if (*p == '$' && (p[1] == '{' || p[1] == '('))
                p = skip_construct(p);
            else if (*p == '`')
                p = skip_construct(p);
            else
                p++;
        }
        if (p > run) {
            char *sub = strndup(run, (size_t)(p - run));
            strbuf_t tmp;
            sb_init(&tmp, 64);
            expand_into(sh, sub ? sub : "", &tmp, 0);
            sb_append(&out, sb_str(&tmp));
            sb_free(&tmp);
            free(sub);
        }
    }

    free(tilded);
    char *result = arena_strdup(sh->scratch, sb_str(&out));
    sb_free(&out);
    return result;
}

char *expand_word_assign(shell_ctx_t *sh, const char *word)
{
    if (!word) return arena_strdup(sh->scratch, "");

    /* In assignment context a tilde expands at the start of the word and after
     * each colon, so that PATH=~/bin:~/sbin works. That means splitting the
     * word on colons -- but only on colons that are actually delimiters.
     *
     * This used to split on every ':' in the string, with no regard for quoting
     * or nesting. So V=${u:-x} was cut into "${u" and "-x}", and the first
     * fragment is an unterminated expansion: "bad substitution". Every colon
     * form of parameter expansion (:- :+ := :?) was unusable in an assignment,
     * while the identical expansion worked as a command argument, in a for
     * list, and in a case word. PREFIX=${PREFIX:-$(pwd)} is a ubiquitous idiom;
     * this is what stopped modernish from bootstrapping.
     */
    strbuf_t result_sb;
    sb_init(&result_sb, 128);

    /* Assignment RHS is a single (non-field-split) word: unquoted $* and $@ join
     * with IFS[0] rather than emitting internal field boundaries. */
    int saved_in_assign = sh->in_assign;
    sh->in_assign = 1;

    const char *p = word;
    const char *seg_start = p;

    for (;;) {
        const char *skipped = skip_construct(p);
        if (skipped != p) {
            p = skipped;                    /* colons inside are not delimiters */
            continue;
        }

        if (*p == ':' || *p == '\0') {
            size_t seg_len = (size_t)(p - seg_start);
            char *segment = strndup(seg_start, seg_len);
            if (segment) {
                char *tilded = expand_tilde(sh, segment, 1);
                const char *src = tilded ? tilded : segment;

                strbuf_t seg_sb;
                sb_init(&seg_sb, 64);
                expand_into(sh, src, &seg_sb, 0);
                sb_append(&result_sb, sb_str(&seg_sb));
                sb_free(&seg_sb);
                free(tilded);
                free(segment);
            }

            if (*p == '\0')
                break;

            sb_appendc(&result_sb, ':');
            seg_start = p + 1;
        }

        p++;
    }

    sh->in_assign = saved_in_assign;
    char *result = arena_strdup(sh->scratch, sb_str(&result_sb));
    sb_free(&result_sb);
    return result;
}

expand_result_t expand_word_full(shell_ctx_t *sh, const char *word)
{
    expand_result_t res;
    res.words = NULL;
    res.count = 0;

    /* Cleared per word; set by expand_into() if this word contains a quoted
     * "$@" and there are no positional parameters. See the nfields == 0 branch
     * below, and shell.h. */
    sh->at_expanded_empty = 0;
    /* Cleared per word; set by expand_into() only when "$@"/"$*" emits a real
     * \x01 field boundary, so the splitter below can tell a boundary from a
     * literal 0x01 byte in the data. See shell.h. */
    sh->at_field_boundary = 0;

    /* Determine whether this word contains unquoted expansions / globs.
     * These checks must be done on the original token text (before expansion),
     * so that quoted chars like "*" in '"*"' are not treated as glob chars. */
    int do_ifs_split = has_unquoted_expansion(word);
    int do_glob      = !sh->opt_f && has_unquoted_glob(word);

    /* Only a word that will be IFS-split needs its quoted sub-regions marked;
     * emitting guards only here means fully-quoted words are byte-for-byte
     * untouched (no marker stripping over their data). Reset per word. */
    sh->emit_guards       = do_ifs_split;
    sh->quote_guard_depth = 0;
    sh->at_quote_guard    = 0;

    char *expanded = expand_word(sh, word);
    sh->emit_guards = 0;
    if (!expanded) {
        char **arr = arena_alloc(sh->scratch, sizeof(char *));
        arr[0] = NULL;
        res.words = arr;
        return res;
    }

    /* If quoted regions were marked, split `expanded` into the guard-free text
     * `plain` plus a parallel `prot` array (prot[i]=1 => byte i was inside a
     * quoted region and must not act as an IFS delimiter). Everything below
     * works on `plain`; only the IFS loop consults `prot`. When no guards were
     * emitted, plain == expanded and prot stays NULL (unchanged behaviour). */
    /* Residual limit: a literal QG_OPEN/QG_CLOSE byte (0x02/0x03) that is itself
     * data inside a quoted region of a splitting word is stripped here, like a
     * literal 0x01 in a "$@" word. Both are control bytes almost never present
     * in real field-split data; the common paths (fully-quoted words, "$@")
     * never reach this code, so their data is untouched. */
    char *prot = NULL;
    if (sh->at_quote_guard) {
        size_t elen = strlen(expanded);
        char *plain = arena_alloc(sh->scratch, elen + 1);
        /* calloc, not malloc: `plain` (and so the split loop's reads of prot)
         * is shorter than elen when guards were stripped, leaving a tail of prot
         * unwritten. The loop never reads it, but zero-initialising keeps that
         * provable (no uninitialised-read path) at negligible cost. */
        prot = calloc(elen + 1, 1);
        size_t w = 0;
        int depth = 0;
        for (size_t r = 0; r < elen; r++) {
            char c = expanded[r];
            if (c == QG_OPEN)  { depth++; continue; }
            if (c == QG_CLOSE) { if (depth > 0) depth--; continue; }
            plain[w] = c;
            if (prot) prot[w] = depth > 0 ? 1 : 0;
            w++;
        }
        plain[w] = '\0';
        expanded = plain;
    }

    /* "$@" word-boundary split: \x01 markers inserted by expand_into for "$@".
     * Gated on at_field_boundary so a literal 0x01 byte in the data (not from
     * "$@") is left intact rather than treated as a field boundary and dropped. */
    if (sh->at_field_boundary && strchr(expanded, '\x01')) {
        char *copy2 = strdup(expanded);
        if (copy2) {
            int cap2 = 4, n2 = 0;
            char **f2 = malloc((size_t)cap2 * sizeof(char *));
            char *tok2 = copy2, *cp2 = copy2;
            while (*cp2) {
                if (*cp2 == '\x01') {
                    *cp2 = '\0';
                    if (n2 >= cap2) {
                        /* `f2 = realloc(f2, ...)` leaks the old block when
                         * realloc fails and returns NULL. Grow via a temporary. */
                        char **g2 = realloc(f2, (size_t)(cap2 * 2) * sizeof(char *));
                        if (!g2) { free(f2); f2 = NULL; break; }
                        cap2 *= 2;
                        f2 = g2;
                    }
                    if (f2) f2[n2++] = pp_decode(sh->scratch, tok2);
                    tok2 = cp2 + 1;
                }
                cp2++;
            }
            if (f2) {
                if (n2 >= cap2) {
                    /* Same realloc-into-self leak as above. */
                    char **g2 = realloc(f2, (size_t)(cap2 * 2) * sizeof(char *));
                    if (!g2) { free(f2); f2 = NULL; }
                    else { f2 = g2; }  /* cap2 not read again on this tail path */
                }
                if (f2) f2[n2++] = pp_decode(sh->scratch, tok2);
                char **arr = arena_alloc(sh->scratch, (size_t)(n2 + 1) * sizeof(char *));
                for (int i = 0; i < n2; i++) arr[i] = f2[i];
                arr[n2] = NULL;
                res.words = arr;
                res.count = n2;
                free(f2);
            }
            free(copy2);
            if (res.words) { free(prot); return res; }
        }
    }

    /* Field splitting on IFS — only for words containing unquoted expansions.
     * POSIX: IFS splitting applies only to results of parameter expansion,
     * command substitution, and arithmetic expansion; not to literal words. */
    const char *ifs = sh_getvar(sh, "IFS");
    if (!ifs) ifs = " \t\n";

    char **fields = NULL;
    int nfields   = 0;
    /* Parallel to fields[]: 1 if field i carries an UNPROTECTED glob
     * metacharacter (so it should undergo pathname expansion even though the
     * word also had a quoted region). Computed during the IFS split where the
     * `prot` map is still aligned to the field bytes. */
    unsigned char *fglob = NULL;

    if (!do_ifs_split) {
        /* "$@" with no positional parameters generates ZERO fields (POSIX
         * 2.5.2), even though it is double-quoted -- unlike "$*", which yields
         * one empty field. This branch is the one that runs for it: the word is
         * quoted, so do_ifs_split is false, and it used to unconditionally
         * manufacture a single empty field here.
         *
         * The guard is on the word expanding to empty, so `a"$@"b` still yields
         * the single field "ab"; only a word that is entirely "$@" disappears.
         *
         * `f "$@"` therefore passed one empty argument instead of none, and
         * `exec cmd "$0" "$@"` -- the autosetup idiom, and sqlite's
         * ./configure -- died with "Unexpected parameter:". */
        if (sh->at_expanded_empty && expanded[0] == '\0')
            goto no_fields;

        /* No IFS splitting: treat the entire expanded string as one field */
        fields = malloc(sizeof(char *));
        if (fields) { fields[0] = expanded; nfields = 1; }
        goto glob_phase;
    }

    /* Count fields */
    char *copy = strdup(expanded);
    if (!copy) {
        char **arr = arena_alloc(sh->scratch, 2 * sizeof(char *));
        arr[0] = expanded;
        arr[1] = NULL;
        res.words = arr;
        res.count = 1;
        free(prot);
        return res;
    }

    /* Split — POSIX 2.6.5.
     *
     * IFS characters are two kinds: whitespace (space/tab/newline that are in
     * IFS) and non-whitespace. A delimiter is a run of IFS whitespace that may
     * contain at most one IFS non-whitespace character; so whitespace ADJACENT
     * to a non-whitespace delimiter is absorbed into it. A whitespace-only
     * delimiter collapses (no empty field); a non-whitespace delimiter preserves
     * the field before it, even when empty. Leading and trailing IFS whitespace
     * are ignored, and a trailing empty field (after the final delimiter) is not
     * produced.
     *
     * The previous loop treated every IFS byte as its own delimiter, so `a :b`
     * with IFS=': ' wrongly yielded a, "", b -- the space acted as a separate
     * delimiter instead of being absorbed into the colon. modernish's FTL_IFS*
     * battery exercises exactly these mixed-IFS cases.
     */
    {
    int cap   = 0;
    char *cp  = copy;
    /* Pointer-based so a byte inside a quoted region (prot[] set) is never a
     * delimiter, even when it is an IFS character -- `${x+"a:b"}` with IFS=:
     * stays one field. prot is indexed by position in `copy` (== position in
     * the guard-free `expanded`). */
#define PROT_AT(ptr) (prot != NULL && prot[(size_t)((ptr) - copy)])
#define IFS_IS(ptr)  (*(ptr) != '\0' && !PROT_AT(ptr) && \
                      strchr(ifs, (unsigned char)*(ptr)) != NULL)
#define IFS_WS(ptr)  (IFS_IS(ptr) && isspace((unsigned char)*(ptr)))
#define IFS_NWS(ptr) (IFS_IS(ptr) && !isspace((unsigned char)*(ptr)))
#define EMIT(s) do {                                                          \
        if (nfields >= cap) {                                                 \
            int ncap_ = cap ? cap * 2 : 8;                                    \
            char **tmp_ = realloc(fields, (size_t)ncap_ * sizeof(char *));    \
            if (!tmp_) { free(copy); goto glob_phase; }                       \
            fields = tmp_;                                                    \
            unsigned char *tg_ = realloc(fglob, (size_t)ncap_ * sizeof(*fglob)); \
            if (!tg_) { free(copy); goto glob_phase; }                        \
            fglob = tg_; cap = ncap_;                                         \
        }                                                                     \
        fglob[nfields] = (unsigned char)                                      \
            field_has_unprotected_glob((s), prot, copy);                      \
        fields[nfields++] = pp_decode(sh->scratch, (s));                     \
    } while (0)

    /* Ignore leading IFS whitespace. */
    while (IFS_WS(cp)) cp++;

    while (*cp) {
        char *fstart = cp;
        while (*cp && !IFS_IS(cp)) cp++;
        char *fend = cp;                 /* field is [fstart, fend) */

        if (*cp == '\0') {
            /* Trailing field: emit only if non-empty (trailing empty dropped). */
            if (fend > fstart) { *fend = '\0'; EMIT(fstart); }
            break;
        }

        /* Consume one delimiter: IFS whitespace, then at most one IFS
         * non-whitespace, then trailing IFS whitespace. */
        int saw_nws = 0;
        while (IFS_WS(cp)) cp++;
        if (IFS_NWS(cp)) { saw_nws = 1; cp++; while (IFS_WS(cp)) cp++; }

        /* A whitespace-only delimiter collapses empty fields; a non-whitespace
         * delimiter preserves the (possibly empty) field before it. */
        if (saw_nws || fend > fstart) { *fend = '\0'; EMIT(fstart); }
    }
#undef PROT_AT
#undef IFS_IS
#undef IFS_WS
#undef IFS_NWS
#undef EMIT
    free(copy);
    }

glob_phase:
    if (nfields == 0) {
        /* POSIX: if an unquoted expansion produces an empty result,
         * it yields zero fields, not one empty field. Only produce an empty
         * field if the original word was literally empty or quoted empty.
         *
         * "$@" with no positional parameters is the other case that yields zero
         * fields, and it is NOT covered by the do_ifs_split test above -- the
         * word is double-quoted, so do_ifs_split is false and the branch below
         * would manufacture one empty field. Note the guard is on the word
         * expanding to empty: `a"$@"b` with no positionals correctly yields the
         * single field "ab", and only a word that is entirely "$@" disappears. */
        if (sh->at_expanded_empty && expanded[0] == '\0')
            goto no_fields;

        if (!do_ifs_split || expanded[0] != '\0') {
            /* Literal word or non-empty result: create single field.
             *
             * nfields used to be set to 1 unconditionally, even when realloc
             * returned NULL -- and the loop below then dereferenced fields[0].
             * The old buffer was leaked on that path too. On OOM, leave the
             * field count at 0 rather than promising a field that isn't there. */
            char **grown = realloc(fields, sizeof(char *));
            if (!grown) {
                free(fields);
                fields  = NULL;
                nfields = 0;
            } else {
                fields    = grown;
                fields[0] = expanded;
                nfields   = 1;
            }
        }
        /* else: empty expansion result from unquoted param expansion yields 0 fields */
    }

no_fields:
    ;   /* a label must precede a statement, not a declaration */

    /* Pathname expansion (globbing) unless set -f */
    char **final = NULL;
    int nfinal = 0;
    int fcap   = 0;

    /* A field undergoes pathname expansion when the pre-expansion word carried a
     * literal unquoted metacharacter (do_glob), OR the field carries an
     * UNPROTECTED metacharacter recorded during the split (fglob[i]) -- typically
     * a variable's VALUE that is a pattern, e.g. `p=*.t; echo $p`, or a mixed
     * word like `"$dir"/$pat`. fglob[i] is set from the `prot` map, so a quoted
     * "*" (protected) still never globs even when the word also has an unquoted
     * expansion. do_glob alone missed value-borne wildcards (the token text has
     * no metacharacter); the old prot==NULL gate missed mixed quoted/unquoted
     * words entirely. */
    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        if (do_glob || (!sh->opt_f && fglob && fglob[i])) {
            glob_t g;
            /* NOT GLOB_NOSORT. POSIX requires pathname expansion results to be
             * sorted, and builds depend on it: a command like `gcc *.c` was
             * being handed files in raw directory order, which varies by
             * filesystem and by the order the files happened to be created.
             * That makes link order -- and therefore the output binary --
             * depend on how the source tree was checked out. */
            int r = glob(f, GLOB_NOCHECK, NULL, &g);
            if (r == 0) {
                for (size_t gi = 0; gi < g.gl_pathc; gi++) {
                    if (nfinal >= fcap) {
                        fcap = fcap ? fcap * 2 : 8;
                        char **tmp = realloc(final, (size_t)fcap * sizeof(char *));
                        if (!tmp) { globfree(&g); goto done; }
                        final = tmp;
                    }
                    final[nfinal++] = arena_strdup(sh->scratch, g.gl_pathv[gi]);
                }
                globfree(&g);
                continue;
            }
            globfree(&g);
        }

        /* No glob or glob failed — keep as-is */
        if (nfinal >= fcap) {
            fcap = fcap ? fcap * 2 : 8;
            char **tmp = realloc(final, (size_t)fcap * sizeof(char *));
            if (!tmp) goto done;
            final = tmp;
        }
        final[nfinal++] = arena_strdup(sh->scratch, f);
    }

done:
    free(fields);
    free(fglob);
    free(prot);

    /* NULL-terminate */
    if (nfinal >= fcap) {
        fcap = nfinal + 1;
        char **tmp = realloc(final, (size_t)fcap * sizeof(char *));
        if (tmp) final = tmp;
    }
    if (final) final[nfinal] = NULL;

    /* Move into arena */
    char **arena_arr = arena_alloc(sh->scratch,
                                   (size_t)(nfinal + 1) * sizeof(char *));
    if (final) {
        memcpy(arena_arr, final, (size_t)(nfinal + 1) * sizeof(char *));
        free(final);
    } else {
        arena_arr[0] = NULL;
    }

    res.words = arena_arr;
    res.count = nfinal;
    return res;
}

char **expand_words(shell_ctx_t *sh, char **words)
{
    if (!words) {
        char **arr = arena_alloc(sh->scratch, sizeof(char *));
        arr[0] = NULL;
        return arr;
    }

    /* First pass: count + collect results */
    int total = 0;

    /* Dynamic list of expand_result_t */
    int nw = 0;
    while (words[nw]) nw++;

    expand_result_t *results = malloc((size_t)nw * sizeof(expand_result_t));
    if (!results) {
        char **arr = arena_alloc(sh->scratch, sizeof(char *));
        arr[0] = NULL;
        return arr;
    }

    for (int i = 0; i < nw; i++) {
        results[i] = expand_word_full(sh, words[i]);
        total += results[i].count;
    }

    char **out = arena_alloc(sh->scratch,
                             (size_t)(total + 1) * sizeof(char *));
    int idx = 0;
    for (int i = 0; i < nw; i++) {
        for (int j = 0; j < results[i].count; j++)
            out[idx++] = results[i].words[j];
    }
    out[idx] = NULL;

    free(results);
    return out;
}
