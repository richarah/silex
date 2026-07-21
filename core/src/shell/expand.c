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
            if (sh->opt_e) sb_appendc(&sb, 'e');
            if (sh->opt_u) sb_appendc(&sb, 'u');
            if (sh->opt_x) sb_appendc(&sb, 'x');
            if (sh->opt_f) sb_appendc(&sb, 'f');
            if (sh->opt_n) sb_appendc(&sb, 'n');
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
    /* Empty expansion ${} is an error */
    if (body[0] == '\0') {
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
        return arena_strdup(sh->scratch, val ? val : "");
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
            return arena_strndup(sh->scratch, s + off, (size_t)avail);
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
                expand_into(sh, word_part, &sb, 0);
                char *r = arena_strdup(sh->scratch, sb_str(&sb));
                sb_free(&sb);
                return r;
            }
            return arena_strdup(sh->scratch, val ? val : "");

        case '+':
            if (!condition) {
                /* Variable is set (and non-empty if colon) — expand word */
                strbuf_t sb;
                sb_init(&sb, 64);
                expand_into(sh, word_part, &sb, 0);
                char *r = arena_strdup(sh->scratch, sb_str(&sb));
                sb_free(&sb);
                return r;
            }
            return arena_strdup(sh->scratch, "");

        case '=':
            if (condition) {
                strbuf_t sb;
                sb_init(&sb, 64);
                expand_into(sh, word_part, &sb, 0);
                const char *newval = sb_str(&sb);
                vars_set(&sh->vars, varname, newval);
                char *r = arena_strdup(sh->scratch, newval);
                sb_free(&sb);
                return r;
            }
            return arena_strdup(sh->scratch, val ? val : "");

        case '?':
            if (condition) {
                strbuf_t sb;
                sb_init(&sb, 64);
                expand_into(sh, word_part, &sb, 0);
                fprintf(stderr, "%s: %s\n", varname,
                        sb_len(&sb) > 0 ? sb_str(&sb) : "parameter null or not set");
                sb_free(&sb);
                exit(1);
            }
            return arena_strdup(sh->scratch, val ? val : "");

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
            return arena_strdup(sh->scratch, s);
        return arena_strdup(sh->scratch, s + off);
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
            return arena_strdup(sh->scratch, s);
        /* Remove suffix starting at off */
        return arena_strndup(sh->scratch, s, (size_t)off);
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
            return arena_strdup(sh->scratch, s);
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
    return arena_strdup(sh->scratch, val ? val : "");
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
        sub.shell_pid    = sh->shell_pid;  /* $$ is the main shell's PID, not the
                                            * command-substitution child's */
        memcpy(sub.funcs, sh->funcs, sizeof(sh->funcs)); /* inherit functions */
        /* Clear set_in_this_shell for inherited traps */
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
} arith_ctx_t;

static void arith_skip_ws(arith_ctx_t *ac)
{
    while (ac->src[ac->pos] == ' ' || ac->src[ac->pos] == '\t')
        ac->pos++;
}

static long arith_expr(arith_ctx_t *ac);
static long arith_ternary(arith_ctx_t *ac);

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

    /* $ variable reference or command substitution */
    if (c == '$') {
        ac->pos++;
        /* $(cmd) command substitution inside arithmetic */
        if (ac->src[ac->pos] == '(') {
            ac->pos++;  /* skip '(' */
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
        /* Could be ${VAR} or $VAR */
        char namebuf[256];
        size_t ni = 0;
        if (ac->src[ac->pos] == '{') {
            ac->pos++;
            while (ac->src[ac->pos] && ac->src[ac->pos] != '}' &&
                   ni < sizeof(namebuf) - 1)
                namebuf[ni++] = ac->src[ac->pos++];
            if (ac->src[ac->pos] == '}') ac->pos++;
        } else if (is_special_var(ac->src[ac->pos]) ||
                   (ac->src[ac->pos] >= '1' && ac->src[ac->pos] <= '9')) {
            /* A special parameter -- $#, $?, $$, $!, $@, $*, $0..$9 -- in
             * arithmetic, e.g. $(($# - 1)). These are single characters that
             * are not name chars, so the loop below read nothing and looked up
             * the empty name, which errored under `set -u` ("unbound variable")
             * instead of yielding the count/status. Read exactly the one char;
             * sh_getvar resolves it. Fixes modernish FTL_HASHVAR. */
            namebuf[ni++] = ac->src[ac->pos++];
        } else {
            while (is_name_char((unsigned char)ac->src[ac->pos]) &&
                   ni < sizeof(namebuf) - 1)
                namebuf[ni++] = ac->src[ac->pos++];
        }
        namebuf[ni] = '\0';
        const char *v = sh_getvar(ac->sh, namebuf);
        if (!v && ac->sh->opt_u) {
            fprintf(stderr, "silex: %s: unbound variable\n", namebuf);
            exit(1);
        }
        return v ? atol(v) : 0L;
    }

    /* Identifier without $ (direct variable name) — may have assignment op */
    if (is_alpha_underscore((unsigned char)c)) {
        char namebuf[256];
        size_t ni = 0;
        while (is_name_char((unsigned char)ac->src[ac->pos]) &&
               ni < sizeof(namebuf) - 1)
            namebuf[ni++] = ac->src[ac->pos++];
        namebuf[ni] = '\0';

        arith_skip_ws(ac);
        const char *p = ac->src + ac->pos;

        /* Post-increment: var++ */
        if (p[0] == '+' && p[1] == '+') {
            ac->pos += 2;
            const char *postinc_v = sh_getvar(ac->sh, namebuf);
            long postinc_old = postinc_v ? atol(postinc_v) : 0L;
            char postinc_nb[32]; snprintf(postinc_nb, sizeof(postinc_nb), "%ld", postinc_old + 1);
            vars_set(&ac->sh->vars, namebuf, postinc_nb);
            return postinc_old;
        }
        /* Post-decrement: var-- */
        if (p[0] == '-' && p[1] == '-') {
            ac->pos += 2;
            const char *postdec_v = sh_getvar(ac->sh, namebuf);
            long postdec_old = postdec_v ? atol(postdec_v) : 0L;
            char postdec_nb[32]; snprintf(postdec_nb, sizeof(postdec_nb), "%ld", postdec_old - 1);
            vars_set(&ac->sh->vars, namebuf, postdec_nb);
            return postdec_old;
        }

        /* Detect assignment operator */
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

        const char *v = sh_getvar(ac->sh, namebuf);
        long cur_val  = v ? atol(v) : 0L;

        if (assign_op) {
            ac->pos += oplen;
            long rhs = arith_ternary(ac);  /* comma has lower precedence than assignment */
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

        /* No assignment operator: just read the value */
        if (!v && ac->sh->opt_u) {
            fprintf(stderr, "silex: %s: unbound variable\n", namebuf);
            exit(1);
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
    ac.src = expr;
    ac.pos = 0;
    ac.sh  = sh;
    return arith_expr(&ac);
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
                /* $( ) cmd substitution — may contain nested "..." */
                p++;
                int d = 1;
                while (*p && d > 0) {
                    if (*p == '\\') { if (p[1]) p += 2; else p++; continue; }
                    if (*p == '\'') {
                        p++;
                        while (*p && *p != '\'') p++;
                        if (*p) p++;
                        continue;
                    }
                    if (*p == '"') { p = skip_dquote_end(p + 1); continue; }
                    if (*p == '(') d++;
                    else if (*p == ')') d--;
                    p++;
                }
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
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0) { sb_appendc(out, '\x01'); sh->at_field_boundary = 1; }
                        sb_append(out, sh->positional[pi]);
                    }
                } else if (*p == '*' && in_dquote) {
                    /* "$*": join with first char of IFS */
                    const char *ifs = sh_getvar(sh, "IFS");
                    char sep = (ifs && ifs[0]) ? ifs[0] : ' ';
                    for (int pi = 0; pi < sh->positional_n; pi++) {
                        if (pi > 0) sb_appendc(out, sep);
                        sb_append(out, sh->positional[pi]);
                    }
                } else {
                    /* Unquoted $* or $@: when IFS is empty, still split on \x01 */
                    const char *ifs = sh_getvar(sh, "IFS");
                    if (ifs && ifs[0] == '\0') {
                        /* Empty IFS: use \x01 to separate positionals */
                        for (int pi = 0; pi < sh->positional_n; pi++) {
                            if (pi > 0) { sb_appendc(out, '\x01'); sh->at_field_boundary = 1; }
                            sb_append(out, sh->positional[pi]);
                        }
                    } else {
                        /* Non-empty or unset IFS: join with space (default behavior) */
                        char spec[2] = { *p, '\0' };
                        const char *v = sh_getvar(sh, spec);
                        if (v) sb_append(out, v);
                    }
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
                if (v) sb_append(out, v);
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
                    if (f2) f2[n2++] = arena_strdup(sh->scratch, tok2);
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
                if (f2) f2[n2++] = arena_strdup(sh->scratch, tok2);
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
            fields = tmp_; cap = ncap_;                                       \
        }                                                                     \
        fields[nfields++] = arena_strdup(sh->scratch, (s));                   \
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

    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        /* Use pre-computed do_glob (based on original token text) to avoid
         * globbing chars that came from quoted contexts like "*". */
        if (do_glob) {
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
