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
            return arena_strdup(&sh->parse_arena, buf);
        case '$':
            snprintf(buf, sizeof(buf), "%ld", (long)getpid());
            return arena_strdup(&sh->parse_arena, buf);
        case '!':
            if (sh->last_bg_pid == 0) return "";
            snprintf(buf, sizeof(buf), "%ld", (long)sh->last_bg_pid);
            return arena_strdup(&sh->parse_arena, buf);
        case '#':
            snprintf(buf, sizeof(buf), "%d", sh->positional_n);
            return arena_strdup(&sh->parse_arena, buf);
        case '-': {
            /* Return current option flags */
            strbuf_t sb;
            sb_init(&sb, 8);
            if (sh->opt_e) sb_appendc(&sb, 'e');
            if (sh->opt_u) sb_appendc(&sb, 'u');
            if (sh->opt_x) sb_appendc(&sb, 'x');
            if (sh->opt_f) sb_appendc(&sb, 'f');
            if (sh->opt_n) sb_appendc(&sb, 'n');
            char *r = arena_strdup(&sh->parse_arena, sb_str(&sb));
            sb_free(&sb);
            return r;
        }
        case '0':
            return sh->script_name ? sh->script_name : "";
        case '@':
        case '*': {
            /* Join all positionals with space */
            strbuf_t sb;
            sb_init(&sb, 64);
            for (int i = 0; i < sh->positional_n; i++) {
                if (i > 0) sb_appendc(&sb, ' ');
                sb_append(&sb, sh->positional[i]);
            }
            char *r = arena_strdup(&sh->parse_arena, sb_str(&sb));
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
        return arena_strdup(&sh->parse_arena, buf);
    }

    return vars_get(&sh->vars, name);
}

/* -------------------------------------------------------------------------
 * Tilde expansion
 * Returns newly-malloc'd string or NULL if no tilde
 * ------------------------------------------------------------------------- */

static char *expand_tilde(shell_ctx_t *sh, const char *word)
{
    if (word[0] != '~')
        return NULL;

    const char *rest = word + 1;
    const char *slash = strchr(rest, '/');
    char username[256];
    size_t ulen = slash ? (size_t)(slash - rest) : strlen(rest);

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
    if (slash) sb_append(&sb, slash);
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
        if (!in_sq && !in_dq &&
            (*p == '*' || *p == '?' || *p == '[')) return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Parameter expansion: ${...} parsing
 * name_start points to char after '$' or after '{'
 * Returns arena-allocated expanded string
 * ------------------------------------------------------------------------- */

/*
 * expand_braced: parse and expand ${...} body.
 * 'body' is the content between { and }, e.g. "VAR:-default" or "#VAR".
 */
static char *expand_braced(shell_ctx_t *sh, const char *body)
{
    /* ${#VAR} — length */
    if (body[0] == '#' && body[1] != '\0' && body[1] != '}') {
        const char *varname = body + 1;
        const char *val = sh_getvar(sh, varname);
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", val ? strlen(val) : (size_t)0);
        return arena_strdup(&sh->parse_arena, buf);
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
    char varname[namelen + 1];
    memcpy(varname, body, namelen);
    varname[namelen] = '\0';

    const char *val = sh_getvar(sh, varname);

    /* No operator: plain ${VAR} */
    if (*p == '\0') {
        return arena_strdup(&sh->parse_arena, val ? val : "");
    }

    /* ${VAR:-word}, ${VAR:+word}, ${VAR:=word}, ${VAR:?word} */
    int colon = (*p == ':');
    if (colon) p++;

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
                expand_into(sh, word_part, &sb, 0);
                char *r = arena_strdup(&sh->parse_arena, sb_str(&sb));
                sb_free(&sb);
                return r;
            }
            return arena_strdup(&sh->parse_arena, val ? val : "");

        case '+':
            if (!condition) {
                /* Variable is set (and non-empty if colon) — expand word */
                strbuf_t sb;
                sb_init(&sb, 64);
                expand_into(sh, word_part, &sb, 0);
                char *r = arena_strdup(&sh->parse_arena, sb_str(&sb));
                sb_free(&sb);
                return r;
            }
            return arena_strdup(&sh->parse_arena, "");

        case '=':
            if (condition) {
                strbuf_t sb;
                sb_init(&sb, 64);
                expand_into(sh, word_part, &sb, 0);
                const char *newval = sb_str(&sb);
                vars_set(&sh->vars, varname, newval);
                char *r = arena_strdup(&sh->parse_arena, newval);
                sb_free(&sb);
                return r;
            }
            return arena_strdup(&sh->parse_arena, val ? val : "");

        case '?':
            if (condition) {
                strbuf_t sb;
                sb_init(&sb, 64);
                expand_into(sh, word_part, &sb, 0);
                fprintf(stderr, "matchbox: %s: %s\n", varname,
                        sb_len(&sb) > 0 ? sb_str(&sb) : "parameter null or not set");
                sb_free(&sb);
                exit(1);
            }
            return arena_strdup(&sh->parse_arena, val ? val : "");

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
        const char *pat = p;
        const char *s   = val ? val : "";
        int off = greedy ? match_prefix(s, pat) : match_prefix_shortest(s, pat);
        if (off < 0)
            return arena_strdup(&sh->parse_arena, s);
        return arena_strdup(&sh->parse_arena, s + off);
    }

    if (op == '%') {
        p++;  /* skip the operator '%' */
        int greedy = (*p == '%');
        if (greedy) p++;
        const char *pat = p;
        const char *s   = val ? val : "";
        int off = greedy ? match_suffix(s, pat) : match_suffix_shortest(s, pat);
        if (off < 0)
            return arena_strdup(&sh->parse_arena, s);
        /* Remove suffix starting at off */
        return arena_strndup(&sh->parse_arena, s, (size_t)off);
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
        size_t i = 0;
        int replaced = 0;
        while (i <= slen) {
            /* Try matching at position i */
            int matched = 0;
            for (size_t mlen = slen - i; ; mlen--) {
                char tmp[mlen + 1];
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
        char *r = arena_strdup(&sh->parse_arena, sb_str(&sb));
        sb_free(&sb);
        return r;
    }

    /* Fallback: return raw value */
    return arena_strdup(&sh->parse_arena, val ? val : "");
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
        return arena_strdup(&sh->parse_arena, "");
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return arena_strdup(&sh->parse_arena, "");
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
        memcpy(sub.funcs, sh->funcs, sizeof(sh->funcs)); /* inherit functions */
        shell_run_string(&sub, cmd);
        int ex = sub.last_exit;
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
    if (WIFEXITED(status))
        sh->last_exit = WEXITSTATUS(status);

    /* Strip trailing newlines (POSIX) */
    while (sb.len > 0 && sb.buf[sb.len - 1] == '\n') {
        sb.len--;
        sb.buf[sb.len] = '\0';
    }

    char *result = arena_strdup(&sh->parse_arena, sb_str(&sb));
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
} arith_ctx_t;

static void arith_skip_ws(arith_ctx_t *ac)
{
    while (ac->src[ac->pos] == ' ' || ac->src[ac->pos] == '\t')
        ac->pos++;
}

static long arith_expr(arith_ctx_t *ac);

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

    /* $ variable reference */
    if (c == '$') {
        ac->pos++;
        /* Could be ${VAR} or $VAR */
        char namebuf[256];
        size_t ni = 0;
        if (ac->src[ac->pos] == '{') {
            ac->pos++;
            while (ac->src[ac->pos] && ac->src[ac->pos] != '}' &&
                   ni < sizeof(namebuf) - 1)
                namebuf[ni++] = ac->src[ac->pos++];
            if (ac->src[ac->pos] == '}') ac->pos++;
        } else {
            while (is_name_char((unsigned char)ac->src[ac->pos]) &&
                   ni < sizeof(namebuf) - 1)
                namebuf[ni++] = ac->src[ac->pos++];
        }
        namebuf[ni] = '\0';
        const char *v = sh_getvar(ac->sh, namebuf);
        return v ? atol(v) : 0L;
    }

    /* Identifier without $ (direct variable name) */
    if (is_alpha_underscore((unsigned char)c)) {
        char namebuf[256];
        size_t ni = 0;
        while (is_name_char((unsigned char)ac->src[ac->pos]) &&
               ni < sizeof(namebuf) - 1)
            namebuf[ni++] = ac->src[ac->pos++];
        namebuf[ni] = '\0';
        const char *v = sh_getvar(ac->sh, namebuf);
        return v ? atol(v) : 0L;
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
        else if (op == '/') left = right ? left / right : 0;
        else left = right ? left % right : 0;
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
        ac->pos++;
        long right = arith_mul(ac);
        if (op == '+') left += right;
        else left -= right;
    }
    return left;
}

static long arith_cmp(arith_ctx_t *ac)
{
    long left = arith_add(ac);
    for (;;) {
        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;
        int op = 0;
        size_t oplen = 1;
        if (p[0] == '<' && p[1] == '=') { op = 1; oplen = 2; }
        else if (p[0] == '>' && p[1] == '=') { op = 2; oplen = 2; }
        else if (p[0] == '<') { op = 3; oplen = 1; }
        else if (p[0] == '>') { op = 4; oplen = 1; }
        else if (p[0] == '=' && p[1] == '=') { op = 5; oplen = 2; }
        else if (p[0] == '!' && p[1] == '=') { op = 6; oplen = 2; }
        else break;
        ac->pos += oplen;
        long right = arith_add(ac);
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

static long arith_expr(arith_ctx_t *ac)
{
    return arith_cmp(ac);
}

long expand_arith(shell_ctx_t *sh, const char *expr)
{
    arith_ctx_t ac;
    ac.src = expr;
    ac.pos = 0;
    ac.sh  = sh;
    return arith_expr(&ac);
}

/* -------------------------------------------------------------------------
 * Core expand_into: walk 'word', handle quoting / substitutions,
 * append expanded text to 'out'.
 * in_dquote: 1 when inside "..."
 * ------------------------------------------------------------------------- */

static void expand_into(shell_ctx_t *sh, const char *word, strbuf_t *out,
                        int in_dquote)
{
    const char *p = word;

    while (*p) {
        /* Single quotes: literal (only outside double-quotes) */
        if (*p == '\'' && !in_dquote) {
            p++; /* skip opening ' */
            while (*p && *p != '\'')
                sb_appendc(out, *p++);
            if (*p == '\'') p++;
            continue;
        }

        /* Double quotes */
        if (*p == '"' && !in_dquote) {
            p++; /* skip opening " */
            expand_into(sh, p, out, 1);
            /* Find matching closing quote */
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '"') depth--;
                else if (*p == '\\' && (p[1] == '"' || p[1] == '\\' ||
                                        p[1] == '$' || p[1] == '`'))
                    p++;
                p++;
            }
            continue;
        }

        /* End of double-quoted section */
        if (*p == '"' && in_dquote) {
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
                sb_append(out, abuf);
                continue;
            }

            /* $(cmd) */
            if (*p == '(') {
                p++;
                const char *start = p;
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    if (depth > 0) p++;
                    else p++;
                }
                size_t clen = (size_t)((p - 1) - start);
                char *cmd = strndup(start, clen);
                char *result = cmd_subst(sh, cmd ? cmd : "");
                free(cmd);
                sb_append(out, result);
                continue;
            }

            /* ${...} */
            if (*p == '{') {
                p++;
                const char *start = p;
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    if (depth > 0) p++;
                    else p++;
                }
                size_t blen = (size_t)((p - 1) - start);
                char *body = strndup(start, blen);
                char *val  = expand_braced(sh, body ? body : "");
                free(body);
                sb_append(out, val);
                continue;
            }

            /* $@ and $* — positional list */
            if (*p == '@' || *p == '*') {
                if (*p == '@' && in_dquote) {
                    /* "$@": each positional as a separate word; use \x01 boundary */
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0) sb_appendc(out, '\x01');
                        sb_append(out, sh->positional[pi]);
                    }
                } else {
                    char spec[2] = { *p, '\0' };
                    const char *v = sh_getvar(sh, spec);
                    if (v) sb_append(out, v);
                }
                p++;
                continue;
            }

            /* Special single-char vars */
            if (is_special_var(*p)) {
                char spec[2] = { *p, '\0' };
                const char *v = sh_getvar(sh, spec);
                if (v) sb_append(out, v);
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
                if (v) sb_append(out, v);
                else if (sh->opt_u) {
                    /* set -u: error on unset var */
                    char tmp[nlen + 1];
                    memcpy(tmp, start, nlen);
                    tmp[nlen] = '\0';
                    fprintf(stderr, "matchbox: %s: unbound variable\n", tmp);
                    exit(1);
                }
                continue;
            }

            /* $1-$9 (already handled in sh_getvar via spec) */
            if (is_digit((unsigned char)*p)) {
                char spec[2] = { *p, '\0' };
                const char *v = sh_getvar(sh, spec);
                if (v) sb_append(out, v);
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
            sb_append(out, result);
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
    if (!word) return arena_strdup(&sh->parse_arena, "");

    /* Tilde expansion first */
    char *tilded = expand_tilde(sh, word);
    const char *src = tilded ? tilded : word;

    strbuf_t sb;
    sb_init(&sb, 128);
    expand_into(sh, src, &sb, 0);
    free(tilded);

    char *result = arena_strdup(&sh->parse_arena, sb_str(&sb));
    sb_free(&sb);
    return result;
}

expand_result_t expand_word_full(shell_ctx_t *sh, const char *word)
{
    expand_result_t res;
    res.words = NULL;
    res.count = 0;

    /* Determine whether this word contains unquoted expansions / globs.
     * These checks must be done on the original token text (before expansion),
     * so that quoted chars like "*" in '"*"' are not treated as glob chars. */
    int do_ifs_split = has_unquoted_expansion(word);
    int do_glob      = !sh->opt_f && has_unquoted_glob(word);

    char *expanded = expand_word(sh, word);
    if (!expanded) {
        char **arr = arena_alloc(&sh->parse_arena, sizeof(char *));
        arr[0] = NULL;
        res.words = arr;
        return res;
    }

    /* "$@" word-boundary split: \x01 markers inserted by expand_into for "$@" */
    if (strchr(expanded, '\x01')) {
        char *copy2 = strdup(expanded);
        if (copy2) {
            int cap2 = 4, n2 = 0;
            char **f2 = malloc((size_t)cap2 * sizeof(char *));
            char *tok2 = copy2, *cp2 = copy2;
            while (*cp2) {
                if (*cp2 == '\x01') {
                    *cp2 = '\0';
                    if (n2 >= cap2) { cap2 *= 2; f2 = realloc(f2, (size_t)cap2 * sizeof(char *)); }
                    if (f2) f2[n2++] = arena_strdup(&sh->parse_arena, tok2);
                    tok2 = cp2 + 1;
                }
                cp2++;
            }
            if (f2) {
                if (n2 >= cap2) { cap2 *= 2; f2 = realloc(f2, (size_t)cap2 * sizeof(char *)); }
                if (f2) f2[n2++] = arena_strdup(&sh->parse_arena, tok2);
                char **arr = arena_alloc(&sh->parse_arena, (size_t)(n2 + 1) * sizeof(char *));
                for (int i = 0; i < n2; i++) arr[i] = f2[i];
                arr[n2] = NULL;
                res.words = arr;
                res.count = n2;
                free(f2);
            }
            free(copy2);
            if (res.words) return res;
        }
    }

    /* Field splitting on IFS — only for words containing unquoted expansions.
     * POSIX: IFS splitting applies only to results of parameter expansion,
     * command substitution, and arithmetic expansion; not to literal words. */
    const char *ifs = sh_getvar(sh, "IFS");
    if (!ifs) ifs = " \t\n";

    char **fields = NULL;
    int nfields   = 0;

    if (!do_ifs_split) {
        /* No IFS splitting: treat the entire expanded string as one field */
        fields = malloc(sizeof(char *));
        if (fields) { fields[0] = expanded; nfields = 1; }
        goto glob_phase;
    }

    /* Count fields */
    char *copy = strdup(expanded);
    if (!copy) {
        char **arr = arena_alloc(&sh->parse_arena, 2 * sizeof(char *));
        arr[0] = expanded;
        arr[1] = NULL;
        res.words = arr;
        res.count = 1;
        return res;
    }

    /* Split */
    {
    int cap       = 0;
    char *tok     = copy;
    char *cp      = copy;

    while (*cp) {
        if (strchr(ifs, (unsigned char)*cp)) {
            if (cp > tok) {
                /* Non-IFS-whitespace separators split even empty fields */
                *cp = '\0';
                if (nfields >= cap) {
                    cap = cap ? cap * 2 : 8;
                    char **tmp = realloc(fields, (size_t)cap * sizeof(char *));
                    if (!tmp) { free(copy); goto glob_phase; }
                    fields = tmp;
                }
                fields[nfields++] = arena_strdup(&sh->parse_arena, tok);
                tok = cp + 1;
            } else {
                tok = cp + 1;
            }
        }
        cp++;
    }
    /* Last field */
    if (*tok) {
        if (nfields >= cap) {
            cap = cap ? cap * 2 : 8;
            char **tmp = realloc(fields, (size_t)cap * sizeof(char *));
            if (tmp) { fields = tmp; }
        }
        if (nfields < cap)
            fields[nfields++] = arena_strdup(&sh->parse_arena, tok);
    }
    free(copy);
    }

glob_phase:
    if (nfields == 0) {
        /* No fields — single empty word */
        fields = realloc(fields, sizeof(char *));
        if (fields) fields[0] = expanded;
        nfields = 1;
    }

    /* Pathname expansion (globbing) unless set -f */
    char **final = NULL;
    int nfinal = 0;
    int fcap   = 0;

    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        /* Use pre-computed do_glob (based on original token text) to avoid
         * globbing chars that came from quoted contexts like "*". */
        if (do_glob) {
            glob_t g;
            int r = glob(f, GLOB_NOSORT | GLOB_NOCHECK, NULL, &g);
            if (r == 0) {
                for (size_t gi = 0; gi < g.gl_pathc; gi++) {
                    if (nfinal >= fcap) {
                        fcap = fcap ? fcap * 2 : 8;
                        char **tmp = realloc(final, (size_t)fcap * sizeof(char *));
                        if (!tmp) { globfree(&g); goto done; }
                        final = tmp;
                    }
                    final[nfinal++] = arena_strdup(&sh->parse_arena, g.gl_pathv[gi]);
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
        final[nfinal++] = arena_strdup(&sh->parse_arena, f);
    }

done:
    free(fields);

    /* NULL-terminate */
    if (nfinal >= fcap) {
        fcap = nfinal + 1;
        char **tmp = realloc(final, (size_t)fcap * sizeof(char *));
        if (tmp) final = tmp;
    }
    if (final) final[nfinal] = NULL;

    /* Move into arena */
    char **arena_arr = arena_alloc(&sh->parse_arena,
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
        char **arr = arena_alloc(&sh->parse_arena, sizeof(char *));
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
        char **arr = arena_alloc(&sh->parse_arena, sizeof(char *));
        arr[0] = NULL;
        return arr;
    }

    for (int i = 0; i < nw; i++) {
        results[i] = expand_word_full(sh, words[i]);
        total += results[i].count;
    }

    char **out = arena_alloc(&sh->parse_arena,
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
