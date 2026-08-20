/* sed.c — sed applet: GNU-compatible stream editor.
 *
 * Implements the POSIX command set plus the GNU extensions the GNU sed
 * testsuite exercises: options -n -e -f -E/-r -i[SUFFIX] -s -z -u -l N
 * --posix --sandbox --follow-symlinks --help --version and long forms;
 * commands D P Q R T W F e v z, 0,/re/ and addr,+N / addr,~N ranges,
 * I/M regex modifiers, s-command flags (g p i I m M e w N), case
 * conversion (\U \L \u \l \E) and \cX \dNNN \oNNN \xHH escapes, and
 * GNU's l-command line wrapping.
 *
 * Error messages follow GNU's format ("sed: -e expression #1, char 8:
 * multiple 'p' options to 's' command") because the testsuite compares
 * them byte-for-byte.  Exit codes follow GNU: 1 for script errors, 2 for
 * unreadable input operands, 4 for I/O errors ("panics").
 *
 * The applet runs IN-PROCESS inside the shell, so nothing here may call
 * exit(): errors longjmp back to applet_sed(), which owns every
 * allocation through the command list and frees it on all paths.
 *
 * POSIX/GNU: an empty regex recalls the last regex EXECUTED at runtime,
 * not the last one compiled (GNU's own testsuite 'recall' case rejects
 * compile-time binding -- and compile-time binding also loops forever on
 * `:x;s//Y/;/f/bx`).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/error.h"
#include "../util/noreturn.h"
#include "../util/path.h"
#include "../util/strbuf.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SED_VERSION_STR "sed (GNU sed) UNKNOWN\nsilex sed applet, emulating the GNU sed command-line interface.\n"

/* Shorten a strbuf in place (strbuf.h has no truncate primitive). */
static void sb_truncate(strbuf_t *sb, size_t len)
{
    if (len < sb->len) {
        sb->len = len;
        sb->buf[len] = '\0';
    }
}

SILEX_NORETURN static void sed_panic(int code, const char *fmt, ...);

/* Drop CR from CR-LF pairs in sb starting at `from` (GNU tolerates DOS
 * line endings in -f script files; subst-options' prog4 ends "\r\n"). */
static void strip_crlf_from(strbuf_t *sb, size_t from)
{
    char *b = sb->buf;
    size_t w = from;
    for (size_t r = from; r < sb->len; r++) {
        if (b[r] == '\r' && r + 1 < sb->len && b[r+1] == '\n')
            continue;
        b[w++] = b[r];
    }
    /* NB: a lone CR not followed by LF is kept -- GNU only tolerates
     * CR-LF pairs, and `s/./x/\r` (no newline) must still be the
     * "unknown option to 's'" error */
    sb->len = w;
    b[w] = '\0';
}

/* sb_init that cannot fail silently: zeroes the struct first (sb_init's
 * failure path leaves fields unset) and panics on allocation failure. */
static void xb_init(strbuf_t *sb, size_t cap)
{
    memset(sb, 0, sizeof(*sb));
    if (sb_init(sb, cap) != 0)
        sed_panic(4, "out of memory");
}

/* ------------------------------------------------------------------ */
/* Global option state (reset on every applet entry)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int   quiet;        /* -n */
    int   ere;          /* -E / -r */
    int   posix;        /* --posix: disable ALL GNU extensions */
    int   posix_env;    /* POSIXLY_CORRECT: semantic changes only (e.g.
                           N at EOF discards); GNU syntax stays enabled */
    int   separate;     /* -s (also implied by -i) */
    int   inplace;      /* -i */
    char *in_suffix;    /* backup suffix for -i (may be NULL) */
    int   nulldata;     /* -z: records are NUL-delimited */
    int   unbuffered;   /* -u */
    int   sandbox;      /* --sandbox */
    int   debug;        /* --debug (accepted; no annotation output) */
    int   follow_syms;  /* --follow-symlinks */
    long  line_len;     /* -l N: default l wrap width (0 = no wrap) */
} sed_opts_t;

static sed_opts_t O;

/* Error unwinding: script/usage errors longjmp here with the exit code. */
static jmp_buf sed_jmp;

SILEX_NORETURN static void sed_panic(int code, const char *fmt, ...)
{
    va_list ap;
    fputs("sed: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    longjmp(sed_jmp, code);
}

/* ------------------------------------------------------------------ */
/* Script assembly: -e/-f segments with source locations              */
/* ------------------------------------------------------------------ */

typedef struct {
    int         is_file;
    int         expr_index;  /* 1-based, for -e segments */
    const char *fname;       /* for -f segments (borrowed from argv) */
    size_t      start;       /* byte offset into the joined script */
} sed_seg_t;

typedef struct {
    const char *src;   /* joined script text */
    const char *p;     /* parse cursor */
    sed_seg_t  *segs;
    int         nsegs;
} parser_t;

/* Format the GNU location prefix for the segment containing offset. */
static void loc_prefix(const parser_t *ps, size_t off, char *buf, size_t bufsz)
{
    const sed_seg_t *seg = &ps->segs[0];
    for (int i = 0; i < ps->nsegs; i++) {
        if (ps->segs[i].start > off) break;
        seg = &ps->segs[i];
    }
    if (seg->is_file) {
        long line = 1;
        for (size_t i = seg->start; i < off && ps->src[i]; i++)
            if (ps->src[i] == '\n') line++;
        snprintf(buf, bufsz, "file %s line %ld", seg->fname, line);
    } else {
        snprintf(buf, bufsz, "-e expression #%d, char %zu",
                 seg->expr_index, off - seg->start);
    }
}

/* Script error at the current parse position; exits 1 (GNU). */
SILEX_NORETURN static void perr(parser_t *ps, const char *fmt, ...)
{
    char where[512], msg[512];
    va_list ap;
    loc_prefix(ps, (size_t)(ps->p - ps->src), where, sizeof(where));
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "sed: %s: %s\n", where, msg);
    longjmp(sed_jmp, 1);
}

/* ------------------------------------------------------------------ */
/* Command representation                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    ADDR_NONE = 0,
    ADDR_LINE,   /* N (0 allowed only as 0,/re/) */
    ADDR_LAST,   /* $ */
    ADDR_REGEX,  /* /re/ or \Cre C */
    ADDR_STEP,   /* first~step (addr1) */
    ADDR_REL,    /* +N (addr2) */
    ADDR_MULT,   /* ~N (addr2) */
} addr_type_t;

typedef struct {
    addr_type_t type;
    long        line;
    long        step;
    regex_t    *re;      /* NULL for the empty regex (runtime recall) */
} sed_addr_t;

/* Replacement tokens for s */
typedef enum { RT_LIT, RT_MATCH, RT_GROUP, RT_CASE } rtok_type_t;
typedef struct {
    rtok_type_t t;
    char       *lit;      /* RT_LIT */
    size_t      len;
    int         grp;      /* RT_GROUP */
    char        op;       /* RT_CASE: U L u l E */
} rtok_t;

/* s flags */
#define SF_G (1 << 0)
#define SF_P (1 << 1)
#define SF_I (1 << 2)
#define SF_M (1 << 3)
#define SF_E (1 << 4)

typedef struct sed_cmd {
    sed_addr_t  a1, a2;
    int         has_a2;
    int         negate;
    int         from0;        /* 0,/re/ range */
    char        cmd;

    /* range state (reset per run for -s) */
    int         active;
    long        end_line;     /* for +N / ~N once activated */

    /* source location for runtime diagnostics */
    size_t      src_off;

    /* s */
    regex_t    *re;           /* NULL: recall last executed regex */
    rtok_t     *rt;
    int         nrt;
    int         sflags;
    int         snth;
    char       *wfile;        /* s///w target (also r/R/w/W filename) */
    FILE       *wfp;

    /* y */
    unsigned char *ymap;      /* 256-byte translation table */

    /* a/i/c text */
    char       *text;
    size_t      tlen;

    /* b/t/T/: label; q/Q code; l width */
    char       *label;
    struct sed_cmd *jump;     /* resolved branch target / matching '}' */
    int         int_arg;

    struct sed_cmd *next;
} sed_cmd_t;

/* Runtime regex recall (last EXECUTED regex). */
static regex_t *g_exec_re;

/* R-command read streams and w-command write streams are shared PER
 * FILENAME across commands: `sed -e 1Rb -e 2Rb` reads successive lines
 * of b, and two `w f` commands append to one open handle. */
typedef struct { const char *name; FILE *fp; int pending_delim; } fstream_t;
static fstream_t g_rstreams[64];
static int       g_nrstreams;
static fstream_t g_wstreams[64];
static int       g_nwstreams;

static void close_streams(void)
{
    for (int i = 0; i < g_nrstreams; i++)
        if (g_rstreams[i].fp)
            fclose(g_rstreams[i].fp);
    g_nrstreams = 0;
    for (int i = 0; i < g_nwstreams; i++)
        if (g_wstreams[i].fp && g_wstreams[i].fp != stdout &&
            g_wstreams[i].fp != stderr)
            fclose(g_wstreams[i].fp);
    g_nwstreams = 0;
}

/* ------------------------------------------------------------------ */
/* Command list lifetime                                              */
/* ------------------------------------------------------------------ */

static void free_cmds(sed_cmd_t *head)
{
    while (head) {
        sed_cmd_t *next = head->next;
        if (head->a1.re) { regfree(head->a1.re); free(head->a1.re); }
        if (head->a2.re) { regfree(head->a2.re); free(head->a2.re); }
        if (head->re)    { regfree(head->re);    free(head->re);    }
        for (int i = 0; i < head->nrt; i++)
            free(head->rt[i].lit);
        free(head->rt);
        free(head->wfile);
        free(head->ymap);
        free(head->text);
        free(head->label);
        free(head);
        head = next;
    }
}

/* ------------------------------------------------------------------ */
/* Escape preprocessing                                               */
/* ------------------------------------------------------------------ */

/* Decode one \-escape at *pp (pointing after the backslash).  Returns the
 * decoded byte and advances *pp, or -1 if the sequence is not a byte
 * escape (caller keeps it verbatim).  ps is used for error reporting. */
static int decode_escape(parser_t *ps, const char **pp)
{
    const char *q = *pp;
    int v;
    switch (*q) {
    case 'n': *pp = q + 1; return '\n';
    case 't': *pp = q + 1; return '\t';
    case 'a': *pp = q + 1; return '\a';
    case 'f': *pp = q + 1; return '\f';
    case 'r': *pp = q + 1; return '\r';
    case 'v': *pp = q + 1; return '\v';
    case 'c':
        /* GNU: dangling \c at end of buffer yields a lone backslash;
         * \c\\ (escaped backslash) is control-backslash (0x1c); \c\X
         * with any other continuation is the recursive-escaping error. */
        if (q[1] == '\0') { *pp = q + 1; return '\\'; }
        if (q[1] == '\\') {
            if (q[2] == '\\') { *pp = q + 3; return '\\' ^ 0x40; }
            perr(ps, "recursive escaping after \\c not allowed");
        }
        v = toupper((unsigned char)q[1]) ^ 0x40;
        *pp = q + 2;
        return v;
    case 'd':
        if (!isdigit((unsigned char)q[1])) return -1;
        v = 0; q++;
        for (int i = 0; i < 3 && isdigit((unsigned char)*q); i++)
            v = v * 10 + (*q++ - '0');
        *pp = q;
        return v & 0xff;
    case 'o':
        if (!(q[1] >= '0' && q[1] <= '7')) return -1;
        v = 0; q++;
        for (int i = 0; i < 3 && *q >= '0' && *q <= '7'; i++)
            v = v * 8 + (*q++ - '0');
        *pp = q;
        return v & 0xff;
    case 'x':
        if (!isxdigit((unsigned char)q[1])) return -1;
        v = 0; q++;
        for (int i = 0; i < 2 && isxdigit((unsigned char)*q); i++) {
            v = v * 16 + (isdigit((unsigned char)*q) ? *q - '0'
                          : tolower((unsigned char)*q) - 'a' + 10);
            q++;
        }
        *pp = q;
        return v & 0xff;
    default:
        return -1;
    }
}

/* Preprocess a raw regex source: byte escapes become literal bytes
 * (backslash-protected when the byte is a regex metacharacter); all other
 * backslash sequences pass through for regcomp.  In POSIX mode, escapes
 * inside bracket expressions are NOT decoded (GNU normalize_text():
 * --posix 's/[\t]/X/' matches backslash-or-t, not tab).  Returns
 * malloc'd text. */
static char *preprocess_re(parser_t *ps, const char *src)
{
    strbuf_t sb;
    xb_init(&sb, strlen(src) + 8);
    const char *q = src;
    int in_br = 0, br_first = 0;
    while (*q) {
        if (in_br) {
            if (br_first && *q == ']') { br_first = 0; sb_appendc(&sb, *q++); continue; }
            br_first = 0;
            if (*q == '[' && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
                char kind = q[1];
                sb_appendc(&sb, *q++); sb_appendc(&sb, *q++);
                while (*q && !(*q == kind && q[1] == ']'))
                    sb_appendc(&sb, *q++);
                if (*q) { sb_appendc(&sb, *q++); sb_appendc(&sb, *q++); }
                continue;
            }
            if (*q == ']') { in_br = 0; sb_appendc(&sb, *q++); continue; }
            if (*q == '\\' && q[1] && O.posix) {
                /* POSIX: keep verbatim inside brackets */
                sb_appendc(&sb, *q++);
                sb_appendc(&sb, *q++);
                continue;
            }
            if (*q == '\\' && q[1]) {
                const char *r = q + 1;
                int b = decode_escape(ps, &r);
                if (b >= 0) { sb_appendc(&sb, (char)b); q = r; continue; }
                sb_appendc(&sb, *q++);
                sb_appendc(&sb, *q++);
                continue;
            }
            sb_appendc(&sb, *q++);
            continue;
        }
        if (*q == '[') {
            in_br = 1;
            sb_appendc(&sb, *q++);
            if (*q == '^') sb_appendc(&sb, *q++);
            br_first = 1;
            continue;
        }
        if (*q == '\\' && q[1]) {
            const char *r = q + 1;
            int b = decode_escape(ps, &r);
            if (b >= 0) {
                if (strchr(".[]*^$\\+?(){}|", b))
                    sb_appendc(&sb, '\\');
                sb_appendc(&sb, (char)b);
                q = r;
                continue;
            }
            sb_appendc(&sb, *q++);
            sb_appendc(&sb, *q++);
            continue;
        }
        sb_appendc(&sb, *q++);
    }
    char *out = strdup(sb_str(&sb));
    sb_free(&sb);
    if (!out) sed_panic(4, "out of memory");
    return out;
}

/* Compile a (preprocessed) regex; on failure report at the current parse
 * position with regerror()'s message, GNU-style. */
static regex_t *compile_re(parser_t *ps, const char *pre, int icase, int mline)
{
    int flags = 0;
    if (O.ere)  flags |= REG_EXTENDED;
    if (icase)  flags |= REG_ICASE;
    if (mline)  flags |= REG_NEWLINE;   /* M: ^/$ match at embedded newlines */
    regex_t *re = malloc(sizeof(*re));
    if (!re) sed_panic(4, "out of memory");
    int rc = regcomp(re, pre, flags);
    if (rc != 0) {
        char ebuf[256];
        regerror(rc, re, ebuf, sizeof(ebuf));
        free(re);
        perr(ps, "%s", ebuf);   /* longjmps; nothing below runs */
    }
    return re;
}

/* ------------------------------------------------------------------ */
/* Delimited reading (regex / replacement / y strings)                */
/* ------------------------------------------------------------------ */

/* Read regex source up to the unescaped delimiter.  Bracket expressions
 * hide the delimiter; \<delim> becomes the literal delimiter character
 * (kept backslash-protected so a metacharacter delimiter stays literal).
 * Consumes the closing delimiter.  what: "address regex" / "'s' command"
 * for the unterminated-error message. */
static char *read_re_until(parser_t *ps, char delim, const char *what)
{
    strbuf_t sb;
    xb_init(&sb, 64);
    int in_br = 0;      /* inside [...] */
    int br_first = 0;   /* position right after [ or [^ */
    while (*ps->p && *ps->p != '\n') {
        char c = *ps->p;
        if (!in_br) {
            if (c == delim) {
                ps->p++;
                char *out = strdup(sb_str(&sb));
                sb_free(&sb);
                if (!out) sed_panic(4, "out of memory");
                return out;
            }
            if (c == '\\' && ps->p[1]) {
                if (ps->p[1] == '\n') {   /* line continuation: literal NL */
                    sb_appendc(&sb, '\\');
                    sb_appendc(&sb, 'n');
                    ps->p += 2;
                    continue;
                }
                if (ps->p[1] == delim) {
                    /* Escaped delimiter: literal character */
                    if (strchr(".[]*^$\\+?(){}|", delim))
                        sb_appendc(&sb, '\\');
                    sb_appendc(&sb, delim);
                    ps->p += 2;
                    continue;
                }
                if (ps->p[1] == 'c' && !O.posix) {
                    /* \cX is a unit: its operand must not open a bracket
                     * or end the pattern (GNU bug#79519: s/\c[// works).
                     * In POSIX mode \c is not special -- warn and let the
                     * '[' be parsed normally. */
                    sb_appendc(&sb, '\\');
                    sb_appendc(&sb, 'c');
                    ps->p += 2;
                    if (*ps->p && *ps->p != delim && *ps->p != '\n') {
                        sb_appendc(&sb, *ps->p);
                        ps->p++;
                    }
                    continue;
                }
                if (ps->p[1] == 'c' && O.posix &&
                    strcmp(what, "'s' command") == 0)
                    fprintf(stderr, "sed: warning: using \"\\c\" in the"
                                    " 's' command is not portable\n");
                sb_appendc(&sb, c);
                sb_appendc(&sb, ps->p[1]);
                ps->p += 2;
                continue;
            }
            if (c == '[') {
                in_br = 1;
                br_first = 1;
                sb_appendc(&sb, c);
                ps->p++;
                if (*ps->p == '^') { sb_appendc(&sb, '^'); ps->p++; }
                continue;
            }
            sb_appendc(&sb, c);
            ps->p++;
        } else {
            if (c == '[' && (ps->p[1] == ':' || ps->p[1] == '.' ||
                             ps->p[1] == '=')) {
                char kind = ps->p[1];
                br_first = 0;   /* a following ']' closes the bracket */
                sb_appendc(&sb, '[');
                sb_appendc(&sb, kind);
                ps->p += 2;
                while (*ps->p && !(*ps->p == kind && ps->p[1] == ']')) {
                    sb_appendc(&sb, *ps->p);
                    ps->p++;
                }
                if (!*ps->p) break;
                sb_appendc(&sb, kind);
                sb_appendc(&sb, ']');
                ps->p += 2;
                continue;
            }
            if (c == ']' && !br_first) {
                in_br = 0;
                sb_appendc(&sb, c);
                ps->p++;
                continue;
            }
            br_first = 0;
            sb_appendc(&sb, c);
            ps->p++;
        }
    }
    sb_free(&sb);
    perr(ps, "unterminated %s", what);
    return NULL; /* unreached */
}

/* Read the raw replacement text of s up to the unescaped delimiter.
 * Backslash-newline embeds a literal newline.  Consumes the delimiter. */
static char *read_repl_until(parser_t *ps, char delim)
{
    strbuf_t sb;
    xb_init(&sb, 64);
    while (*ps->p) {
        char c = *ps->p;
        if (c == delim) {
            ps->p++;
            char *out = strdup(sb_str(&sb));
            sb_free(&sb);
            if (!out) sed_panic(4, "out of memory");
            return out;
        }
        if (c == '\n')
            break;
        if (c == '\\' && ps->p[1]) {
            if (ps->p[1] == delim) {
                sb_appendc(&sb, delim);
                ps->p += 2;
                continue;
            }
            if (ps->p[1] == '\n') {
                sb_appendc(&sb, '\n');
                ps->p += 2;
                continue;
            }
            sb_appendc(&sb, c);
            sb_appendc(&sb, ps->p[1]);
            ps->p += 2;
            continue;
        }
        sb_appendc(&sb, c);
        ps->p++;
    }
    sb_free(&sb);
    perr(ps, "unterminated 's' command");
    return NULL; /* unreached */
}

/* Parse a raw replacement string into tokens (compile time). */
static void parse_replacement(parser_t *ps, sed_cmd_t *cmd, const char *raw)
{
    int cap = 8, n = 0;
    rtok_t *rt = calloc((size_t)cap, sizeof(rtok_t));
    if (!rt) sed_panic(4, "out of memory");
    cmd->rt = rt;      /* owned by cmd immediately (error-safe) */
    strbuf_t lit;
    xb_init(&lit, 32);

#define FLUSH_LIT() do {                                             \
        if (sb_len(&lit) > 0) {                                      \
            if (n == cap) { cap *= 2;                                \
                rt = realloc(rt, (size_t)cap * sizeof(rtok_t));      \
                if (!rt) { sb_free(&lit); sed_panic(4, "out of memory"); } \
                memset(rt + n, 0, (size_t)(cap - n) * sizeof(rtok_t)); \
                cmd->rt = rt; }                                      \
            rt[n].t = RT_LIT;                                        \
            rt[n].len = sb_len(&lit);                                \
            rt[n].lit = malloc(rt[n].len + 1);                       \
            if (!rt[n].lit) { sb_free(&lit); sed_panic(4, "out of memory"); } \
            memcpy(rt[n].lit, sb_str(&lit), rt[n].len + 1);          \
            n++; cmd->nrt = n;                                       \
            sb_reset(&lit);                                          \
        }                                                            \
    } while (0)

#define ADD_TOK(ty, field, val) do {                                 \
        FLUSH_LIT();                                                 \
        if (n == cap) { cap *= 2;                                    \
            rt = realloc(rt, (size_t)cap * sizeof(rtok_t));          \
            if (!rt) { sb_free(&lit); sed_panic(4, "out of memory"); } \
            memset(rt + n, 0, (size_t)(cap - n) * sizeof(rtok_t));   \
            cmd->rt = rt; }                                          \
        rt[n].t = (ty);                                              \
        rt[n].field = (val);                                         \
        n++; cmd->nrt = n;                                           \
    } while (0)

    const char *q = raw;
    while (*q) {
        if (*q == '&') {
            ADD_TOK(RT_MATCH, grp, 0);
            q++;
            continue;
        }
        if (*q == '\\' && q[1]) {
            char e = q[1];
            if (e >= '0' && e <= '9') {
                int grp = e - '0';
                ADD_TOK(RT_GROUP, grp, grp);
                if (grp > 0) {
                    /* validity of \N checked at compile below */
                }
                q += 2;
                continue;
            }
            if (e == 'U' || e == 'L' || e == 'u' || e == 'l' || e == 'E') {
                if (O.posix) {
                    /* GNU-only escape: literal char plus a warning */
                    fprintf(stderr, "sed: warning: using \"\\%c\" in the"
                                    " 's' command is not portable\n", e);
                    sb_appendc(&lit, e);
                    q += 2;
                    continue;
                }
                ADD_TOK(RT_CASE, op, e);
                q += 2;
                continue;
            }
            if (e == '&' || e == '\\') {
                sb_appendc(&lit, e);
                q += 2;
                continue;
            }
            if (O.posix)
                /* POSIX-portable escapes are \delim, \\, \&, \digit --
                 * anything else draws GNU's portability warning */
                fprintf(stderr, "sed: warning: using \"\\%c\" in the 's'"
                                " command is not portable\n", e);
            {
                const char *r = q + 1;
                int b = decode_escape(ps, &r);
                if (b >= 0) {
                    sb_appendc(&lit, (char)b);
                    q = r;
                    continue;
                }
            }
            /* Unknown escape: the character itself, backslash dropped */
            sb_appendc(&lit, e);
            q += 2;
            continue;
        }
        sb_appendc(&lit, *q++);
    }
    FLUSH_LIT();
    sb_free(&lit);
#undef FLUSH_LIT
#undef ADD_TOK
}

/* ------------------------------------------------------------------ */
/* Address parsing                                                    */
/* ------------------------------------------------------------------ */

/* Try to parse an address at the cursor.  Returns 1 if one was parsed. */
static int parse_addr(parser_t *ps, sed_addr_t *addr, int is_second)
{
    memset(addr, 0, sizeof(*addr));

    if (*ps->p == '$') {
        addr->type = ADDR_LAST;
        ps->p++;
        return 1;
    }
    if ((*ps->p == '+' || *ps->p == '~') &&
        isdigit((unsigned char)ps->p[1]) && !O.posix) {
        char kind = *ps->p;
        if (!is_second) {
            ps->p += 2;
            perr(ps, "invalid usage of +N or ~N as first address");
        }
        ps->p++;
        char *end;
        addr->line = strtol(ps->p, &end, 10);
        ps->p = end;
        addr->type = (kind == '+') ? ADDR_REL : ADDR_MULT;
        return 1;
    }
    if (isdigit((unsigned char)*ps->p)) {
        char *end;
        addr->line = strtol(ps->p, &end, 10);
        ps->p = end;
        if (*ps->p == '~' && !is_second && !O.posix) {
            ps->p++;
            addr->step = strtol(ps->p, &end, 10);
            ps->p = end;
            addr->type = ADDR_STEP;
        } else {
            addr->type = ADDR_LINE;
        }
        return 1;
    }
    if (*ps->p == '/' || (*ps->p == '\\' && ps->p[1])) {
        char delim = '/';
        if (*ps->p == '\\') {
            ps->p++;
            delim = *ps->p;
        }
        ps->p++;
        char *raw = read_re_until(ps, delim, "address regex");
        /* I/M modifiers */
        int icase = 0, mline = 0;
        for (;;) {
            if (*ps->p == 'I') { icase = 1; ps->p++; }
            else if (*ps->p == 'M') { mline = 1; ps->p++; }
            else break;
        }
        if (raw[0] == '\0') {
            if (icase || mline) {
                free(raw);
                perr(ps, "cannot specify modifiers on empty regexp");
            }
            free(raw);
            addr->re = NULL;   /* runtime recall */
        } else {
            char *pre = preprocess_re(ps, raw);
            free(raw);
            addr->re = compile_re(ps, pre, icase, mline);
            free(pre);
        }
        addr->type = ADDR_REGEX;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Whitespace / command termination helpers                           */
/* ------------------------------------------------------------------ */

static void skip_ws(parser_t *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t')
        ps->p++;
}

/* After a completed command: only ; \n } # or end of script may follow. */
static void expect_cmd_end(parser_t *ps)
{
    skip_ws(ps);
    if (*ps->p == '\0' || *ps->p == '\n' || *ps->p == ';' ||
        *ps->p == '}' || *ps->p == '#')
        return;
    ps->p++;
    perr(ps, "extra characters after command");
}

/* Read a label / branch target: to whitespace, ';', '}', '#' or newline. */
static char *read_label(parser_t *ps)
{
    skip_ws(ps);
    const char *start = ps->p;
    while (*ps->p && *ps->p != '\n' && *ps->p != ';' &&
           *ps->p != ' ' && *ps->p != '\t' && *ps->p != '}' &&
           *ps->p != '#')
        ps->p++;
    size_t len = (size_t)(ps->p - start);
    char *out = malloc(len + 1);
    if (!out) sed_panic(4, "out of memory");
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* Read a filename: one optional leading blank run, then to end of line. */
static char *read_filename(parser_t *ps)
{
    skip_ws(ps);
    const char *start = ps->p;
    while (*ps->p && *ps->p != '\n')
        ps->p++;
    size_t len = (size_t)(ps->p - start);
    while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t'))
        len--;
    if (len == 0)
        perr(ps, "missing filename in r/R/w/W commands");
    char *out = malloc(len + 1);
    if (!out) sed_panic(4, "out of memory");
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* Read a/i/c text.  GNU accepts three forms:
 *   a text            (one-liner; not in --posix mode)
 *   a\ [newline] text (POSIX; backslash-newline continues)
 *   a\text            (text on the same line after the backslash)         */
static void read_text_arg(parser_t *ps, sed_cmd_t *cmd, char cname)
{
    strbuf_t sb;
    xb_init(&sb, 64);
    if (*ps->p == '\\') {
        ps->p++;
        if (*ps->p == '\0') {
            /* `a\` at end of script: GNU appends nothing at all (not an
             * empty line) -- neutralize the command */
            sb_free(&sb);
            cmd->cmd = '#';
            cmd->text = strdup("");
            cmd->tlen = 0;
            if (!cmd->text) sed_panic(4, "out of memory");
            return;
        }
        if (O.posix && *ps->p == '\n') {
            /* In POSIX mode each -e must be a complete command: `-e a\`
             * cannot take its text from the NEXT -e (GNU: "incomplete
             * command").  The joining newline is one that starts a
             * segment. */
            size_t off = (size_t)(ps->p - ps->src) + 1;
            for (int si = 0; si < ps->nsegs; si++) {
                if (ps->segs[si].start == off && !ps->segs[si].is_file) {
                    sb_free(&sb);
                    perr(ps, "incomplete command");
                }
            }
        }
        if (*ps->p == '\n')
            ps->p++;
        /* leading whitespace of the first text line is preserved by GNU
         * after a backslash-newline; after "a\" GNU strips nothing */
    } else {
        if (O.posix || *ps->p == '\0') {
            /* POSIX requires the backslash form; and even GNU rejects a
             * bare `a` with no text at end of script */
            if (*ps->p)
                ps->p++;
            sb_free(&sb);
            perr(ps, "expected \\ after 'a', 'c' or 'i'");
        }
        skip_ws(ps);
    }
    for (;;) {
        while (*ps->p && *ps->p != '\n' && *ps->p != '\\') {
            sb_appendc(&sb, *ps->p);
            ps->p++;
        }
        if (*ps->p == '\\' && ps->p[1] == '\n') {
            sb_appendc(&sb, '\n');
            ps->p += 2;
            continue;
        }
        if (*ps->p == '\\' && ps->p[1]) {
            /* GNU keeps other escapes as the escaped character */
            sb_appendc(&sb, ps->p[1]);
            ps->p += 2;
            continue;
        }
        break;
    }
    if (*ps->p == '\n')
        ps->p++;
    (void)cname;
    cmd->tlen = sb_len(&sb);
    cmd->text = malloc(cmd->tlen + 1);
    if (!cmd->text) { sb_free(&sb); sed_panic(4, "out of memory"); }
    memcpy(cmd->text, sb_str(&sb), cmd->tlen + 1);
    sb_free(&sb);
}

/* ------------------------------------------------------------------ */
/* Script parser                                                      */
/* ------------------------------------------------------------------ */

/* Commands that accept no / one / two addresses. */
static int max_addrs(char c)
{
    switch (c) {
    case ':': case '#': case '}': return 0;
    case 'q': case 'Q': return 1;   /* '=' takes 2 in GNU mode; the POSIX
                                       one-address rule is checked at the
                                       call site */
    default: return 2;
    }
}

static sed_cmd_t *parse_script(parser_t *ps, sed_cmd_t ***tail_out)
{
    sed_cmd_t *head = NULL;
    sed_cmd_t **tail = &head;
    sed_cmd_t *brace_stack[64];
    int depth = 0;

    /* A script whose first two characters are exactly "#n" implies -n;
     * the rest of that line is still a comment (GNU: '#ni!' activates,
     * '# n' and '#N' do not). */
    if (ps->p[0] == '#' && ps->p[1] == 'n') {
        O.quiet = 1;
        ps->p += 2;
        while (*ps->p && *ps->p != '\n')   /* rest of the line: comment */
            ps->p++;
    }

    for (;;) {
        while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
               *ps->p == ';')
            ps->p++;
        if (*ps->p == '\0')
            break;
        if (*ps->p == '#') {
            while (*ps->p && *ps->p != '\n')
                ps->p++;
            continue;
        }

        sed_cmd_t *cmd = calloc(1, sizeof(sed_cmd_t));
        if (!cmd) sed_panic(4, "out of memory");
        *tail = cmd;                 /* linked immediately: error-safe */
        tail = &cmd->next;
        cmd->src_off = (size_t)(ps->p - ps->src);

        int naddr = 0;
        if (parse_addr(ps, &cmd->a1, 0)) {
            naddr = 1;
            skip_ws(ps);
            if (*ps->p == ',') {
                ps->p++;
                skip_ws(ps);
                if (!parse_addr(ps, &cmd->a2, 1)) {
                    ps->p++;
                    perr(ps, "unexpected ','");
                }
                cmd->has_a2 = 1;
                naddr = 2;
            }
        }
        skip_ws(ps);

        while (*ps->p == '!') {
            if (cmd->negate) {
                ps->p++;
                perr(ps, "multiple '!'s");
            }
            cmd->negate = 1;
            ps->p++;
            skip_ws(ps);
        }

        char c = *ps->p;
        if (c == '\0') {
            if (naddr > 0)
                perr(ps, "missing command");
            break;
        }
        ps->p++;
        cmd->cmd = c;

        /* Address-arity validation, GNU wording */
        if (c == '#') {
            if (naddr > 0)
                perr(ps, "comments don't accept any addresses");
        } else if (c == ':') {
            if (naddr > 0)
                perr(ps, ": doesn't want any addresses");
        } else if (c == '}') {
            /* depth is checked first: `1}` is "unexpected '}'" */
            if (depth == 0)
                perr(ps, "unexpected '}'");
            if (naddr > 0)
                perr(ps, "'}' doesn't want any addresses");
        } else if (naddr > max_addrs(c) ||
                   (naddr > 1 && O.posix && strchr("ail=r", c))) {
            /* POSIX allows only one address for a/i/l/=/r */
            perr(ps, "command only uses one address");
        }

        /* 0,/re/ validity.  A single address 0 is also accepted for r/R
         * (GNU: `sed '0r header' file` prepends the file). */
        if (cmd->a1.type == ADDR_LINE && cmd->a1.line == 0) {
            if (cmd->has_a2) {
                if (O.posix || cmd->a2.type != ADDR_REGEX)
                    perr(ps, "invalid usage of line address 0");
                cmd->from0 = 1;
            } else if (O.posix || (c != 'r' && c != 'R')) {
                perr(ps, "invalid usage of line address 0");
            }
        }

        switch (c) {
        case '{':
            if (depth < 64)
                brace_stack[depth] = cmd;
            depth++;
            break;
        case '}':
            if (depth == 0)
                perr(ps, "unexpected '}'");
            depth--;
            if (depth < 64)
                brace_stack[depth]->jump = cmd;
            /* '}' needs a separator before the next command: GNU rejects
             * `1{}d` with "extra characters after command" */
            expect_cmd_end(ps);
            break;

        case 'p': case 'd': case 'g': case 'G': case 'h': case 'H':
        case 'x': case 'n': case 'N': case 'D': case 'P': case '=':
            expect_cmd_end(ps);
            break;

        case 'z': case 'F':
            if (O.posix)
                perr(ps, "unknown command: '%c'", c);
            expect_cmd_end(ps);
            break;

        case 'q': case 'Q': {
            if (c == 'Q' && O.posix)
                perr(ps, "unknown command: '%c'", c);
            skip_ws(ps);
            cmd->int_arg = -1;
            if (isdigit((unsigned char)*ps->p)) {
                char *end;
                cmd->int_arg = (int)strtol(ps->p, &end, 10);
                ps->p = end;
            }
            expect_cmd_end(ps);
            break;
        }

        case 'l': {
            skip_ws(ps);
            cmd->int_arg = -1;
            if (isdigit((unsigned char)*ps->p)) {
                if (O.posix) { /* 'l N' is a GNU extension */
                    ps->p++;   /* GNU reports the char after the digit */
                    perr(ps, "extra characters after command");
                }
                char *end;
                cmd->int_arg = (int)strtol(ps->p, &end, 10);
                ps->p = end;
            }
            expect_cmd_end(ps);
            break;
        }

        case ':':
            cmd->label = read_label(ps);
            if (cmd->label[0] == '\0')
                perr(ps, "\":\" lacks a label");
            break;

        case 'b': case 't': case 'T':
            if (c == 'T' && O.posix)
                perr(ps, "unknown command: '%c'", c);
            cmd->label = read_label(ps);
            break;

        case 'a': case 'i': case 'c':
            read_text_arg(ps, cmd, c);
            break;

        case 'r': case 'R': case 'w': case 'W':
            if ((c == 'R' || c == 'W') && O.posix)
                perr(ps, "unknown command: '%c'", c);
            if (O.sandbox)
                perr(ps, "e/r/w commands disabled in sandbox mode");
            cmd->wfile = read_filename(ps);
            break;

        case 'e': {
            if (O.posix)
                perr(ps, "unknown command: '%c'", c);
            if (O.sandbox)
                perr(ps, "e/r/w commands disabled in sandbox mode");
            skip_ws(ps);
            const char *start = ps->p;
            while (*ps->p && *ps->p != '\n')
                ps->p++;
            cmd->tlen = (size_t)(ps->p - start);
            cmd->text = malloc(cmd->tlen + 1);
            if (!cmd->text) sed_panic(4, "out of memory");
            memcpy(cmd->text, start, cmd->tlen);
            cmd->text[cmd->tlen] = '\0';
            break;
        }

        case 'v': {
            if (O.posix)
                perr(ps, "unknown command: '%c'", c);
            /* version assertion: reject requests for a newer major */
            skip_ws(ps);
            const char *vstart = ps->p;
            while (*ps->p && *ps->p != '\n' && *ps->p != ';')
                ps->p++;
            if (isdigit((unsigned char)*vstart)) {
                long major = strtol(vstart, NULL, 10);
                if (major > 4)
                    perr(ps, "expected newer version of sed");
            }
            break;
        }

        case 's': {
            if (*ps->p == '\0' || *ps->p == '\n')
                perr(ps, "unterminated 's' command");
            char delim = *ps->p++;
            char *pat = read_re_until(ps, delim, "'s' command");
            char *raw_repl = NULL;
            /* Hold pat in the cmd's wfile slot is wrong; keep local but
             * free before any perr via structured handling below. */
            raw_repl = read_repl_until(ps, delim);   /* may longjmp: pat leaks
                                                        only on error exit */
            parse_replacement(ps, cmd, raw_repl);
            free(raw_repl);

            /* flags */
            cmd->snth = 0;
            int seen_g = 0, seen_p = 0, seen_num = 0;
            int icase = 0, mline = 0;
            for (;;) {
                char f = *ps->p;
                if (f == 'g') {
                    ps->p++;
                    if (seen_g) { free(pat); perr(ps, "multiple 'g' options to 's' command"); }
                    seen_g = 1;
                    cmd->sflags |= SF_G;
                } else if (f == 'p') {
                    ps->p++;
                    if (seen_p) { free(pat); perr(ps, "multiple 'p' options to 's' command"); }
                    seen_p = 1;
                    cmd->sflags |= SF_P;
                } else if (f == 'i' || f == 'I') {
                    ps->p++;
                    if (O.posix) { free(pat); perr(ps, "unknown option to 's'"); }
                    icase = 1;
                } else if (f == 'm' || f == 'M') {
                    ps->p++;
                    if (O.posix) { free(pat); perr(ps, "unknown option to 's'"); }
                    mline = 1;
                    cmd->sflags |= SF_M;
                } else if (f == 'e') {
                    ps->p++;
                    if (O.posix) { free(pat); perr(ps, "unknown option to 's'"); }
                    if (O.sandbox) { free(pat); perr(ps, "e/r/w commands disabled in sandbox mode"); }
                    cmd->sflags |= SF_E;
                    if (cmd->sflags & SF_P)
                        cmd->int_arg = 1;   /* p seen before e: print first */
                } else if (isdigit((unsigned char)f)) {
                    char *end;
                    long v = strtol(ps->p, &end, 10);
                    ps->p = end;
                    if (seen_num) { free(pat); perr(ps, "multiple number options to 's' command"); }
                    seen_num = 1;
                    if (v == 0) { free(pat); perr(ps, "number option to 's' command may not be zero"); }
                    cmd->snth = (int)v;
                } else if (f == 'w') {
                    ps->p++;
                    if (O.sandbox) { free(pat); perr(ps, "e/r/w commands disabled in sandbox mode"); }
                    cmd->wfile = read_filename(ps);
                    break;
                } else if (f == '\0' || f == '\n' || f == ';' ||
                           f == '}' || f == '#' || f == ' ' || f == '\t') {
                    break;
                } else {
                    ps->p++;
                    free(pat);
                    perr(ps, "unknown option to 's'");
                }
            }
            if (cmd->snth == 0)
                cmd->snth = 1;

            if (pat[0] == '\0') {
                if (icase || mline) {
                    free(pat);
                    perr(ps, "cannot specify modifiers on empty regexp");
                }
                cmd->re = NULL;   /* runtime recall */
            } else {
                char *pre = preprocess_re(ps, pat);
                cmd->re = compile_re(ps, pre, icase, mline);
                free(pre);
            }
            /* validate \N group references now that re is known
             * (GNU skips this validation in POSIX mode) */
            if (cmd->re && !O.posix) {
                for (int i = 0; i < cmd->nrt; i++) {
                    if (cmd->rt[i].t == RT_GROUP &&
                        (size_t)cmd->rt[i].grp > cmd->re->re_nsub) {
                        int bad = cmd->rt[i].grp;
                        free(pat);
                        perr(ps, "invalid reference \\%d on 's' command's RHS", bad);
                    }
                }
            }
            free(pat);
            expect_cmd_end(ps);
            break;
        }

        case 'y': {
            if (*ps->p == '\0' || *ps->p == '\n')
                perr(ps, "unterminated 'y' command");
            char delim = *ps->p++;
            /* SPLIT first (a backslash escapes the next char, notably the
             * delimiter), THEN decode escapes within each half.  Decoding
             * while splitting would let \c swallow the closing delimiter;
             * GNU's match_slash/convert split the same way, which is why
             * `y/a/\c/` is a length error, not an unterminated command. */
            strbuf_t half[2];
            xb_init(&half[0], 32);
            xb_init(&half[1], 32);
            int ok = 1;
            for (int h = 0; h < 2 && ok; h++) {
                strbuf_t raw;
                xb_init(&raw, 32);
                for (;;) {
                    char yc = *ps->p;
                    if (yc == '\0' || yc == '\n') { ok = 0; break; }
                    if (yc == delim) { ps->p++; break; }
                    if (yc == '\\' && ps->p[1]) {
                        sb_appendc(&raw, yc);
                        sb_appendc(&raw, ps->p[1]);
                        ps->p += 2;
                        continue;
                    }
                    sb_appendc(&raw, yc);
                    ps->p++;
                }
                /* decode pass */
                const char *q = sb_str(&raw);
                while (*q) {
                    if (*q == '\\' && q[1]) {
                        char e = q[1];
                        if (e == delim) { sb_appendc(&half[h], delim); q += 2; continue; }
                        if (e == '\\')  { sb_appendc(&half[h], '\\');  q += 2; continue; }
                        if (e == '\n')  { sb_appendc(&half[h], '\n');  q += 2; continue; }
                        if (e == 'c' && q[2] == '\0') {
                            /* dangling \c in y stays two characters
                             * (GNU: `y/a/\c/` is a LENGTH error) */
                            sb_appendc(&half[h], '\\');
                            sb_appendc(&half[h], 'c');
                            q += 2;
                            continue;
                        }
                        const char *r = q + 1;
                        int b = decode_escape(ps, &r);
                        if (b >= 0) { sb_appendc(&half[h], (char)b); q = r; continue; }
                        sb_appendc(&half[h], e);
                        q += 2;
                        continue;
                    }
                    if (*q == '\\' && !q[1]) {
                        /* dangling backslash (e.g. from a dangling \c):
                         * a literal backslash */
                        sb_appendc(&half[h], '\\');
                        q++;
                        continue;
                    }
                    sb_appendc(&half[h], *q++);
                }
                sb_free(&raw);
            }
            if (!ok) {
                sb_free(&half[0]);
                sb_free(&half[1]);
                perr(ps, "unterminated 'y' command");
            }
            if (sb_len(&half[0]) != sb_len(&half[1])) {
                sb_free(&half[0]);
                sb_free(&half[1]);
                perr(ps, "'y' command strings have different lengths");
            }
            cmd->ymap = malloc(256);
            if (!cmd->ymap) { sb_free(&half[0]); sb_free(&half[1]); sed_panic(4, "out of memory"); }
            for (int i = 0; i < 256; i++)
                cmd->ymap[i] = (unsigned char)i;
            const char *s0 = sb_str(&half[0]);
            const char *s1 = sb_str(&half[1]);
            for (size_t i = 0; i < sb_len(&half[0]); i++)
                cmd->ymap[(unsigned char)s0[i]] = (unsigned char)s1[i];
            sb_free(&half[0]);
            sb_free(&half[1]);
            expect_cmd_end(ps);
            break;
        }

        default:
            perr(ps, "unknown command: '%c'", c);
        }
    }

    if (depth > 0) {
        /* GNU reports char 0 for a pending '{' at end of script */
        ps->p = ps->src;
        perr(ps, "unmatched '{'");
    }

    /* Resolve branch targets */
    for (sed_cmd_t *cmd2 = head; cmd2; cmd2 = cmd2->next) {
        if ((cmd2->cmd == 'b' || cmd2->cmd == 't' || cmd2->cmd == 'T') &&
            cmd2->label && cmd2->label[0]) {
            sed_cmd_t *dest = head;
            for (; dest; dest = dest->next)
                if (dest->cmd == ':' && dest->label &&
                    strcmp(dest->label, cmd2->label) == 0)
                    break;
            if (!dest) {
                fprintf(stderr, "sed: can't find label for jump to '%s'\n",
                        cmd2->label);
                longjmp(sed_jmp, 1);
            }
            cmd2->jump = dest;
        }
    }

    if (tail_out) *tail_out = tail;
    return head;
}

/* ------------------------------------------------------------------ */
/* Execution                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int   type;         /* 0 = text, 1 = whole file (r), 2 = one line (R) */
    char *text;         /* borrowed from cmd for text; NULL otherwise */
    size_t len;
    sed_cmd_t *cmd;     /* for r/R: filename + persistent stream */
} append_t;

typedef struct {
    strbuf_t ps, hs;
    int      ps_had_delim;
    int      hs_had_delim;   /* the chomped flag travels with content
                                through h/H/g/G/x (GNU line.chomped) */

    long     linenum;
    int      last_line;      /* current record is the last one */
    int      tflag;          /* substitution since last input/branch */

    /* input */
    int      need_lookahead;  /* script uses $: peek one record ahead */
    char   **files;
    int      nfiles;
    int      cur;            /* current file index (continuous mode) */
    FILE    *fp;
    const char *cur_name;
    char    *peek;
    size_t   peek_len;
    int      peek_had_delim;
    int      have_peek;
    int      input_open_failed;   /* any operand unreadable -> exit 2 */

    /* output */
    FILE    *out;
    int      pending_nl;     /* last record printed without its delimiter */

    /* append queue */
    append_t aq[512];
    int      naq;

    int      quit_code;      /* >= 0 once q/Q executed */
    int      quit_noprint;
} exec_t;

static char REC_DELIM = '\n';

/* R command streams (persist across cycles) */
static void out_flush_pending(exec_t *st)
{
    if (st->pending_nl) {
        fputc(REC_DELIM, st->out);
        st->pending_nl = 0;
    }
}

static void out_data(exec_t *st, const char *data, size_t len)
{
    out_flush_pending(st);
    fwrite(data, 1, len, st->out);
}

static void out_record(exec_t *st, const char *data, size_t len, int had_delim)
{
    out_flush_pending(st);
    fwrite(data, 1, len, st->out);
    if (had_delim)
        fputc(REC_DELIM, st->out);
    else
        st->pending_nl = 1;
}

static void out_text_line(exec_t *st, const char *data, size_t len)
{
    out_flush_pending(st);
    fwrite(data, 1, len, st->out);
    fputc(REC_DELIM, st->out);
}

/* Write the pattern space to a w-target with the same delayed-delimiter
 * rule as the main output: a record that arrived without its final
 * delimiter is written without one, unless more output follows. */
static void wfile_write(exec_t *st, sed_cmd_t *cmd, const char *data,
                        size_t len, int had_delim)
{
    FILE *fp;
    if (strcmp(cmd->wfile, "/dev/stdout") == 0) {
        out_record(st, data, len, had_delim);
        return;
    }
    fstream_t *fs = NULL;
    for (int i = 0; i < g_nwstreams; i++)
        if (strcmp(g_wstreams[i].name, cmd->wfile) == 0) { fs = &g_wstreams[i]; break; }
    if (!fs) {
        if (strcmp(cmd->wfile, "/dev/stderr") == 0)
            fp = stderr;
        else {
            fp = fopen(cmd->wfile, "w");
            if (!fp)
                sed_panic(4, "couldn't open file %s: %s", cmd->wfile,
                          strerror(errno));
        }
        if (g_nwstreams < (int)(sizeof(g_wstreams)/sizeof(g_wstreams[0]))) {
            g_wstreams[g_nwstreams].name = cmd->wfile;
            g_wstreams[g_nwstreams].fp = fp;
            g_wstreams[g_nwstreams].pending_delim = 0;
            fs = &g_wstreams[g_nwstreams];
            g_nwstreams++;
        }
    }
    fp = fs ? fs->fp : stderr;
    if (fs && fs->pending_delim) {
        fputc(REC_DELIM, fp);
        fs->pending_delim = 0;
    }
    fwrite(data, 1, len, fp);
    if (had_delim)
        fputc(REC_DELIM, fp);
    else if (fs)
        fs->pending_delim = 1;
    if (O.unbuffered || fp == stderr)
        fflush(fp);
}

/* ---- input ---- */

/* Resolve a symlink chain the way GNU's follow_symlinks() does: readlink
 * iteratively, keeping relative names relative ('la2' -> 'la1' -> 'a'). */
static const char *resolve_symlinks(const char *name, char *buf, size_t bufsz)
{
    char target[PATH_MAX];
    snprintf(buf, bufsz, "%s", name);
    for (int depth = 0; depth < 32; depth++) {
        ssize_t n = readlink(buf, target, sizeof(target) - 1);
        if (n < 0) {
            if (errno == EINVAL || errno == ENOTDIR)
                return buf;               /* not a symlink: resolved */
            sed_panic(4, "couldn't readlink %s: %s", buf, strerror(errno));
        }
        target[n] = '\0';
        if (target[0] == '/') {
            snprintf(buf, bufsz, "%s", target);
        } else {
            char dir[PATH_MAX];
            path_dirname(buf, dir);
            if (strcmp(dir, ".") == 0)
                snprintf(buf, bufsz, "%s", target);
            else
                snprintf(buf, bufsz, "%s/%s", dir, target);
        }
    }
    return buf;
}

static FILE *open_input(exec_t *st, const char *name)
{
    /* "-" is stdin -- except for in-place editing, where it names a
     * real file called "-" (GNU in-place-hyphen). */
    if (strcmp(name, "-") == 0 && !O.inplace) {
        st->cur_name = "-";
        if (O.unbuffered)
            setvbuf(stdin, NULL, _IONBF, 0);
        return stdin;
    }
    static char linkbuf[PATH_MAX];
    if (O.follow_syms)
        name = resolve_symlinks(name, linkbuf, sizeof(linkbuf));
    FILE *fp = fopen(name, "r");
    if (!fp) {
        fprintf(stderr, "sed: can't read %s: %s\n", name, strerror(errno));
        st->input_open_failed = 1;
        return NULL;
    }
    /* -u: byte-at-a-time reads so a following reader on the same fd sees
     * the rest of the input; otherwise a fat buffer for getdelim(). */
    setvbuf(fp, NULL, O.unbuffered ? _IONBF : _IOFBF,
            O.unbuffered ? 0 : 65536);
    st->cur_name = name;
    return fp;
}

/* Fetch the next raw record into (*buf,*len); continuous mode walks the
 * file list.  Returns 1 on success. */
static int fetch_record(exec_t *st, char **buf, size_t *len, int *had_delim)
{
    static char *line;
    static size_t cap;
    for (;;) {
        if (!st->fp) {
            while (st->cur < st->nfiles) {
                st->fp = open_input(st, st->files[st->cur]);
                st->cur++;
                if (st->fp) break;
            }
            if (!st->fp)
                return 0;
        }
        ssize_t n = getdelim(&line, &cap, REC_DELIM, st->fp);
        if (n >= 0) {
            int hd = (n > 0 && line[n-1] == REC_DELIM);
            if (hd) n--;
            *buf = line;
            *len = (size_t)n;
            *had_delim = hd;
            return 1;
        }
        if (ferror(st->fp))
            sed_panic(4, "read error on %s: %s", st->cur_name, strerror(errno));
        if (st->fp != stdin)
            fclose(st->fp);
        st->fp = NULL;
        if (st->cur >= st->nfiles)
            return 0;
    }
}

/* Load next record into the pattern space (with lookahead for $). */
static int next_record(exec_t *st)
{
    char *buf; size_t len; int hd;
    if (st->have_peek) {
        sb_reset(&st->ps);
        sb_appendn(&st->ps, st->peek, st->peek_len);
        st->ps_had_delim = st->peek_had_delim;
        st->have_peek = 0;
    } else {
        if (!fetch_record(st, &buf, &len, &hd))
            return 0;
        sb_reset(&st->ps);
        sb_appendn(&st->ps, buf, len);
        st->ps_had_delim = hd;
    }
    st->linenum++;
    /* Peek for last-line detection -- but ONLY when the script uses $.
     * Reading ahead otherwise would steal input from whatever reads the
     * descriptor after us (`sed -u 1q` must leave line 2 unconsumed). */
    if (st->need_lookahead) {
        if (fetch_record(st, &buf, &len, &hd)) {
            free(st->peek);
            st->peek = malloc(len + 1);
            if (!st->peek) sed_panic(4, "out of memory");
            memcpy(st->peek, buf, len);
            st->peek[len] = '\0';
            st->peek_len = len;
            st->peek_had_delim = hd;
            st->have_peek = 1;
            st->last_line = 0;
        } else {
            st->last_line = 1;
        }
    }
    st->tflag = 0;
    return 1;
}

/* ---- append queue ---- */

static void queue_append_text(exec_t *st, char *text, size_t len)
{
    if (st->naq >= (int)(sizeof(st->aq)/sizeof(st->aq[0])))
        return;
    st->aq[st->naq].type = 0;
    st->aq[st->naq].text = text;
    st->aq[st->naq].len  = len;
    st->naq++;
}

static void queue_append_cmd(exec_t *st, sed_cmd_t *cmd, int type)
{
    if (st->naq >= (int)(sizeof(st->aq)/sizeof(st->aq[0])))
        return;
    st->aq[st->naq].type = type;
    st->aq[st->naq].cmd  = cmd;
    st->naq++;
}

static FILE *get_rfile(sed_cmd_t *cmd);

static void flush_appends(exec_t *st)
{
    for (int i = 0; i < st->naq; i++) {
        append_t *a = &st->aq[i];
        if (a->type == 0) {
            out_text_line(st, a->text, a->len);
        } else if (a->type == 1) {
            FILE *fp = fopen(a->cmd->wfile, "r");
            if (fp) {
                char buf[8192];
                size_t n;
                out_flush_pending(st);
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
                    fwrite(buf, 1, n, st->out);
                fclose(fp);
            }
        } else {
            /* R: one line per invocation from a stream shared by all R
             * commands naming this file */
            FILE *fp = get_rfile(a->cmd);
            if (fp) {
                char *ln = NULL;
                size_t cap = 0;
                ssize_t n = getline(&ln, &cap, fp);
                if (n > 0) {
                    out_flush_pending(st);
                    fwrite(ln, 1, (size_t)n, st->out);
                    if (ln[n-1] != '\n')
                        fputc('\n', st->out);
                }
                free(ln);
            }
        }
    }
    st->naq = 0;
}

/* ---- w-file handling ---- */

/* Shared read stream for R (NULL after EOF or open failure). */
static FILE *get_rfile(sed_cmd_t *cmd)
{
    for (int i = 0; i < g_nrstreams; i++)
        if (g_rstreams[i].name &&
            strcmp(g_rstreams[i].name, cmd->wfile) == 0)
            return g_rstreams[i].fp;
    FILE *fp = fopen(cmd->wfile, "r");   /* missing: silently ignored */
    if (g_nrstreams < (int)(sizeof(g_rstreams)/sizeof(g_rstreams[0]))) {
        g_rstreams[g_nrstreams].name = cmd->wfile;
        g_rstreams[g_nrstreams].fp = fp;
        g_nrstreams++;
    }
    return fp;
}

/* ---- runtime regex helpers ---- */

/* Match re against the byte range [start,end) of s.
 *
 * REG_STARTEND is the BSD/glibc extension that expresses this directly: the
 * subject is bounded by offsets rather than by a NUL, so embedded NULs in the
 * pattern space are ordinary bytes, and the offsets handed back stay absolute
 * (relative to s, not to start).  Anchoring is the caller's business via
 * REG_NOTBOL, exactly as it is with REG_STARTEND.
 *
 * musl has no REG_STARTEND and no other length-taking regexec, so there it is
 * emulated by matching a NUL-terminated copy of the range and shifting the
 * offsets back.  The one thing the copy cannot reproduce is a NUL *inside* the
 * range: musl's regexec stops there, so on a musl build a NUL truncates the
 * subject.  glibc builds are unaffected -- this compiles to the plain
 * REG_STARTEND call.
 */
static int sed_regexec_range(const regex_t *re, const char *s,
                             size_t start, size_t end,
                             size_t nmatch, regmatch_t *pmatch, int eflags)
{
#ifdef REG_STARTEND
    pmatch[0].rm_so = (regoff_t)start;
    pmatch[0].rm_eo = (regoff_t)end;
    return regexec(re, s, nmatch, pmatch, eflags | REG_STARTEND);
#else
    size_t len = end - start;
    char *tmp = malloc(len + 1);
    if (!tmp) return REG_ESPACE;
    memcpy(tmp, s + start, len);
    tmp[len] = '\0';

    int rc = regexec(re, tmp, nmatch, pmatch, eflags);
    if (rc == 0) {
        /* Re-base onto s so callers see absolute offsets either way. */
        for (size_t i = 0; i < nmatch; i++) {
            if (pmatch[i].rm_so >= 0) {
                pmatch[i].rm_so += (regoff_t)start;
                pmatch[i].rm_eo += (regoff_t)start;
            }
        }
    }
    free(tmp);
    return rc;
#endif
}

static regex_t *effective_re(exec_t *st, regex_t *re, size_t src_off,
                             const parser_t *ps)
{
    (void)st;
    if (re) {
        g_exec_re = re;
        return re;
    }
    if (!g_exec_re) {
        char where[512];
        /* GNU reports char 0 for this runtime error */
        const sed_seg_t *seg = &ps->segs[0];
        for (int i = 0; i < ps->nsegs; i++) {
            if (ps->segs[i].start > src_off) break;
            seg = &ps->segs[i];
        }
        if (seg->is_file)
            snprintf(where, sizeof(where), "file %s line %d", seg->fname, 0);
        else
            snprintf(where, sizeof(where), "-e expression #%d, char 0",
                     seg->expr_index);
        fprintf(stderr, "sed: %s: no previous regular expression\n", where);
        longjmp(sed_jmp, 1);
    }
    return g_exec_re;
}

static int addr_match_one(exec_t *st, sed_addr_t *a, size_t src_off,
                          const parser_t *psinfo)
{
    switch (a->type) {
    case ADDR_LINE: return st->linenum == a->line;
    case ADDR_LAST: return st->last_line;
    case ADDR_STEP:
        if (a->step <= 0) return st->linenum == a->line;
        if (st->linenum < a->line) return 0;
        return (st->linenum - a->line) % a->step == 0;
    case ADDR_REGEX: {
        regex_t *re = effective_re(st, a->re, src_off, psinfo);
        /* Length-based matching so embedded NULs in the pattern space do
         * not truncate the subject (see sed_regexec_range). */
        regmatch_t m0;
        int r = sed_regexec_range(re, sb_str(&st->ps), 0, sb_len(&st->ps),
                                  1, &m0, 0) == 0;
        g_exec_re = re;
        return r;
    }
    default: return 0;
    }
}

/* Full address evaluation with range state.  *range_last is set when this
 * line is the final line of an active range (for c). */
static int cmd_matches(exec_t *st, sed_cmd_t *cmd, const parser_t *psinfo,
                       int *range_last)
{
    int m;
    *range_last = 1;
    if (cmd->a1.type == ADDR_NONE) {
        m = 1;
    } else if (!cmd->has_a2) {
        m = addr_match_one(st, &cmd->a1, cmd->src_off, psinfo);
    } else if (!cmd->active) {
        int start;
        if (cmd->from0) {
            /* 0,/re/: the range is active from line 1; addr2 may match the
             * very first line.  Line 0 can never match again, so once the
             * range has ended it never restarts (end_line doubles as the
             * "already ran" latch, set when the range closes below). */
            start = (cmd->end_line == 0);
        } else {
            start = addr_match_one(st, &cmd->a1, cmd->src_off, psinfo);
        }
        m = 0;
        if (start) {
            m = 1;
            *range_last = 0;
            switch (cmd->a2.type) {
            case ADDR_LINE:
                if (cmd->a2.line <= st->linenum)
                    *range_last = 1;           /* one-line range */
                else
                    cmd->active = 1;
                break;
            case ADDR_REL:
                cmd->end_line = st->linenum + cmd->a2.line;
                if (cmd->end_line <= st->linenum)
                    *range_last = 1;
                else
                    cmd->active = 1;
                break;
            case ADDR_MULT: {
                long n = cmd->a2.line;
                long end = st->linenum;
                if (n > 0) {
                    end = ((st->linenum + n - 1) / n) * n;
                    if (end == st->linenum && st->linenum % n != 0)
                        end += n;
                }
                cmd->end_line = end;
                if (end <= st->linenum)
                    *range_last = 1;
                else
                    cmd->active = 1;
                break;
            }
            case ADDR_LAST:
                if (st->last_line)
                    *range_last = 1;
                else
                    cmd->active = 1;
                break;
            case ADDR_REGEX:
                if (cmd->from0 &&
                    addr_match_one(st, &cmd->a2, cmd->src_off, psinfo)) {
                    *range_last = 1;   /* /re/ matched line 1 itself */
                    cmd->end_line = 1; /* from0 latch: never restart */
                } else {
                    cmd->active = 1;
                }
                break;
            default:
                cmd->active = 1;
                break;
            }
        }
    } else {
        /* inside an active range */
        m = 1;
        *range_last = 0;
        switch (cmd->a2.type) {
        case ADDR_LINE:
            if (st->linenum >= cmd->a2.line) {
                cmd->active = 0;
                *range_last = 1;
            }
            break;
        case ADDR_REL:
        case ADDR_MULT:
            if (st->linenum >= cmd->end_line) {
                cmd->active = 0;
                *range_last = 1;
            }
            break;
        case ADDR_LAST:
            if (st->last_line) {
                cmd->active = 0;
                *range_last = 1;
            }
            break;
        case ADDR_REGEX:
            if (addr_match_one(st, &cmd->a2, cmd->src_off, psinfo)) {
                cmd->active = 0;
                *range_last = 1;
                if (cmd->from0)
                    cmd->end_line = 1;   /* from0 latch: never restart */
            }
            break;
        default:
            break;
        }
        if (st->last_line) cmd->active = 0;
    }
    if (cmd->negate)
        m = !m;
    return m;
}

/* ---- s execution ---- */

static void append_cased(strbuf_t *sb, const char *data, size_t len,
                         char *case_mode, char *one_shot)
{
    for (size_t i = 0; i < len; i++) {
        int ch = (unsigned char)data[i];
        if (*one_shot) {
            ch = (*one_shot == 'u') ? toupper(ch) : tolower(ch);
            *one_shot = 0;
        } else if (*case_mode) {
            ch = (*case_mode == 'U') ? toupper(ch) : tolower(ch);
        }
        sb_appendc(sb, (char)ch);
    }
}

static int do_subst(exec_t *st, sed_cmd_t *cmd, const parser_t *psinfo)
{
    regex_t *re = effective_re(st, cmd->re, cmd->src_off, psinfo);
    const char *s = sb_str(&st->ps);
    size_t slen = sb_len(&st->ps);
    regmatch_t m[10];
    strbuf_t out;
    xb_init(&out, slen + 16);

    /* -z + M flag: GNU anchors ^/$ at NUL delimiters (DFA_EOL_NUL).
     * regexec can't, so match against a copy with NUL and NL swapped;
     * offsets are unaffected and all output text is taken from the
     * original buffer. */
    const char *subject = s;
    char *swapped = NULL;
    if (O.nulldata && (cmd->sflags & SF_M) && slen > 0) {
        swapped = malloc(slen + 1);
        if (swapped) {
            for (size_t i = 0; i < slen; i++)
                swapped[i] = s[i] == '\0' ? '\n'
                           : s[i] == '\n' ? '\0' : s[i];
            swapped[slen] = '\0';
            subject = swapped;
        }
    }

    size_t pos = 0;
    int count = 0, made = 0;
    char case_mode = 0, one_shot = 0;
    size_t prev_end = (size_t)-1;   /* end of the previous match */

    while (pos <= slen) {
        /* Match within [pos,slen) of the buffer itself so embedded NULs are
         * ordinary bytes; offsets come back absolute (see sed_regexec_range). */
        int ef = (pos > 0 ? REG_NOTBOL : 0);
        if (sed_regexec_range(re, subject, pos, slen, 10, m, ef) != 0)
            break;
        size_t so = (size_t)m[0].rm_so;
        size_t eo = (size_t)m[0].rm_eo;
        /* An empty match immediately after a previous match doesn't
         * count: GNU's global a-star to x substitution on "bac" gives
         * xbxcx, not xbxxcx. */
        if (eo == so && so == prev_end) {
            if (so >= slen)
                break;
            sb_appendc(&out, s[so]);
            pos = so + 1;
            continue;
        }
        count++;
        int replace = (cmd->sflags & SF_G) ? (count >= cmd->snth)
                                           : (count == cmd->snth);
        if (replace) {
            sb_appendn(&out, s + pos, so - pos);
            case_mode = 0; one_shot = 0;
            for (int i = 0; i < cmd->nrt; i++) {
                rtok_t *rt = &cmd->rt[i];
                switch (rt->t) {
                case RT_LIT:
                    append_cased(&out, rt->lit, rt->len, &case_mode, &one_shot);
                    break;
                case RT_MATCH:
                    append_cased(&out, s + so, eo - so, &case_mode, &one_shot);
                    break;
                case RT_GROUP: {
                    int g = rt->grp;
                    if (m[g].rm_so >= 0)   /* offsets are absolute */
                        append_cased(&out, s + (size_t)m[g].rm_so,
                                     (size_t)(m[g].rm_eo - m[g].rm_so),
                                     &case_mode, &one_shot);
                    break;
                }
                case RT_CASE:
                    if (rt->op == 'E')      { case_mode = 0; one_shot = 0; }
                    else if (rt->op == 'U') case_mode = 'U';
                    else if (rt->op == 'L') case_mode = 'L';
                    else if (rt->op == 'u') one_shot = 'u';
                    else if (rt->op == 'l') one_shot = 'l';
                    break;
                }
            }
            made = 1;
        } else {
            sb_appendn(&out, s + pos, eo - pos);
        }
        prev_end = eo;
        if (eo == so) {
            if (eo >= slen) { pos = eo; break; }
            sb_appendc(&out, s[eo]);
            pos = eo + 1;
        } else {
            pos = eo;
        }
        if (!(cmd->sflags & SF_G) && count >= cmd->snth)
            break;
    }
    sb_appendn(&out, s + pos, slen - pos);
    free(swapped);

    if (made) {
        sb_reset(&st->ps);
        sb_appendn(&st->ps, sb_str(&out), sb_len(&out));
        st->tflag = 1;
    }
    sb_free(&out);
    return made;
}

/* ---- e command / s///e flag ---- */

static void run_shell_into(exec_t *st, const char *cmdline, strbuf_t *dst)
{
    fflush(st->out);
    FILE *fp = popen(cmdline, "r");
    if (!fp)
        return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (dst)
            sb_appendn(dst, buf, n);
        else
            fwrite(buf, 1, n, st->out);
    }
    pclose(fp);
}

/* ---- l command ---- */

static void do_list(exec_t *st, long width)
{
    const char *s = sb_str(&st->ps);
    size_t len = sb_len(&st->ps);
    out_flush_pending(st);
    long col = 0;
    char tok[8];
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int tl;
        switch (c) {
        case '\\': memcpy(tok, "\\\\", 3); tl = 2; break;
        case '\a': memcpy(tok, "\\a", 3);  tl = 2; break;
        case '\b': memcpy(tok, "\\b", 3);  tl = 2; break;
        case '\f': memcpy(tok, "\\f", 3);  tl = 2; break;
        case '\n': memcpy(tok, "\\n", 3);  tl = 2; break;
        case '\r': memcpy(tok, "\\r", 3);  tl = 2; break;
        case '\t': memcpy(tok, "\\t", 3);  tl = 2; break;
        case '\v': memcpy(tok, "\\v", 3);  tl = 2; break;
        default:
            if (c < 32 || c >= 127) {
                snprintf(tok, sizeof(tok), "\\%03o", c);
                tl = 4;
            } else {
                tok[0] = (char)c; tok[1] = '\0';
                tl = 1;
            }
        }
        if (width > 1 && col + tl > width - 1) {
            fputs("\\\n", st->out);
            col = 0;
        }
        fwrite(tok, 1, (size_t)tl, st->out);
        col += tl;
    }
    fputc('$', st->out);
    fputc(REC_DELIM, st->out);
}

/* ---- main per-stream execution ---- */

/* Returns the exit code contribution (0 normally). */
static void run_stream(sed_cmd_t *head, exec_t *st, const parser_t *psinfo)
{
    /* GNU prepend idiom: a single address 0 on r queues the file before
     * the first input line. */
    for (sed_cmd_t *c = head; c; c = c->next) {
        if ((c->cmd == 'r' || c->cmd == 'R') && !c->negate &&
            c->a1.type == ADDR_LINE && c->a1.line == 0 && !c->has_a2)
            queue_append_cmd(st, c, c->cmd == 'r' ? 1 : 2);
    }
    if (st->naq > 0)
        flush_appends(st);

    if (!next_record(st))
        return;

    for (;;) {
        sed_cmd_t *cmd = head;
        int cycle_print = 1;

    restart_no_read:
        while (cmd) {
            int range_last;
            if (!cmd_matches(st, cmd, psinfo, &range_last)) {
                if (cmd->cmd == '{' && cmd->jump)
                    cmd = cmd->jump;   /* skip to matching '}' */
                cmd = cmd->next;
                continue;
            }
            switch (cmd->cmd) {
            case '{': case '}': case ':': case '#': case 'v':
                break;

            case 'p':
                out_record(st, sb_str(&st->ps), sb_len(&st->ps),
                           st->ps_had_delim);
                break;

            case 'P': {
                const char *nl = memchr(sb_str(&st->ps), REC_DELIM, sb_len(&st->ps));
                size_t n = nl ? (size_t)(nl - sb_str(&st->ps))
                              : sb_len(&st->ps);
                out_text_line(st, sb_str(&st->ps), n);
                break;
            }

            case 'd':
                cycle_print = 0;
                goto cycle_end;

            case 'D': {
                const char *nl = memchr(sb_str(&st->ps), REC_DELIM, sb_len(&st->ps));
                if (!nl) {
                    cycle_print = 0;
                    goto cycle_end;
                }
                size_t skip = (size_t)(nl - sb_str(&st->ps)) + 1;
                size_t rest = sb_len(&st->ps) - skip;
                memmove((char *)sb_str(&st->ps), sb_str(&st->ps) + skip, rest);
                sb_truncate(&st->ps, rest);
                cmd = head;
                goto restart_no_read;
            }

            case 'g':
                sb_reset(&st->ps);
                sb_appendn(&st->ps, sb_str(&st->hs), sb_len(&st->hs));
                st->ps_had_delim = st->hs_had_delim;
                break;

            case 'G':
                sb_appendc(&st->ps, REC_DELIM);
                sb_appendn(&st->ps, sb_str(&st->hs), sb_len(&st->hs));
                st->ps_had_delim = st->hs_had_delim;
                break;

            case 'h':
                sb_reset(&st->hs);
                sb_appendn(&st->hs, sb_str(&st->ps), sb_len(&st->ps));
                st->hs_had_delim = st->ps_had_delim;
                break;

            case 'H':
                sb_appendc(&st->hs, REC_DELIM);
                sb_appendn(&st->hs, sb_str(&st->ps), sb_len(&st->ps));
                st->hs_had_delim = st->ps_had_delim;
                break;

            case 'x': {
                strbuf_t tmp = st->ps;
                st->ps = st->hs;
                st->hs = tmp;
                int td = st->ps_had_delim;
                st->ps_had_delim = st->hs_had_delim;
                st->hs_had_delim = td;
                break;
            }

            case 'z':
                sb_reset(&st->ps);
                break;

            case '=': {
                char nbuf[32];
                int n = snprintf(nbuf, sizeof(nbuf), "%ld", st->linenum);
                out_data(st, nbuf, (size_t)n);
                fputc(REC_DELIM, st->out);
                break;
            }

            case 'F': {
                out_data(st, st->cur_name, strlen(st->cur_name));
                fputc(REC_DELIM, st->out);
                break;
            }

            case 'l':
                do_list(st, cmd->int_arg >= 0 ? cmd->int_arg : O.line_len);
                break;

            case 'q':
                st->quit_code = cmd->int_arg >= 0 ? cmd->int_arg : 0;
                goto cycle_end;

            case 'Q':
                st->quit_code = cmd->int_arg >= 0 ? cmd->int_arg : 0;
                st->quit_noprint = 1;
                cycle_print = 0;
                goto cycle_end;

            case 'n':
                if (!O.quiet)
                    out_record(st, sb_str(&st->ps), sb_len(&st->ps),
                               st->ps_had_delim);
                flush_appends(st);
                if (!next_record(st)) {
                    cycle_print = 0;
                    goto stream_end;
                }
                break;

            case 'N': {
                flush_appends(st);
                strbuf_t save;
                xb_init(&save, sb_len(&st->ps) + 1);
                sb_appendn(&save, sb_str(&st->ps), sb_len(&st->ps));
                int save_delim = st->ps_had_delim;
                if (!next_record(st)) {
                    /* GNU: N at EOF prints pattern space (unless -n) and
                     * exits; POSIX mode exits without printing. */
                    sb_free(&save);
                    st->ps_had_delim = save_delim;
                    if (O.posix || O.posix_env)
                        cycle_print = 0;
                    goto cycle_end_final;
                }
                sb_appendc(&save, REC_DELIM);
                sb_appendn(&save, sb_str(&st->ps), sb_len(&st->ps));
                sb_reset(&st->ps);
                sb_appendn(&st->ps, sb_str(&save), sb_len(&save));
                sb_free(&save);
                break;
            }

            case 'a':
                queue_append_text(st, cmd->text, cmd->tlen);
                break;

            case 'i':
                out_text_line(st, cmd->text, cmd->tlen);
                break;

            case 'c':
                cycle_print = 0;
                if (range_last)
                    out_text_line(st, cmd->text, cmd->tlen);
                goto cycle_end;

            case 'r':
                queue_append_cmd(st, cmd, 1);
                break;

            case 'R':
                queue_append_cmd(st, cmd, 2);
                break;

            case 'w':
                wfile_write(st, cmd, sb_str(&st->ps), sb_len(&st->ps),
                            st->ps_had_delim);
                break;

            case 'W': {
                const char *nl = memchr(sb_str(&st->ps), REC_DELIM, sb_len(&st->ps));
                size_t n = nl ? (size_t)(nl - sb_str(&st->ps))
                              : sb_len(&st->ps);
                wfile_write(st, cmd, sb_str(&st->ps), n,
                            nl ? 1 : st->ps_had_delim);
                break;
            }

            case 'e': {
                if (cmd->tlen > 0) {
                    run_shell_into(st, cmd->text, NULL);
                } else {
                    strbuf_t res;
                    xb_init(&res, 128);
                    run_shell_into(st, sb_str(&st->ps), &res);
                    if (sb_len(&res) > 0 &&
                        sb_str(&res)[sb_len(&res)-1] == '\n')
                        sb_truncate(&res, sb_len(&res) - 1);
                    sb_reset(&st->ps);
                    sb_appendn(&st->ps, sb_str(&res), sb_len(&res));
                    sb_free(&res);
                }
                break;
            }

            case 's': {
                int made = do_subst(st, cmd, psinfo);
                if (made) {
                    /* p and e apply in the order they were written:
                     * s///pe prints the unexecuted text, s///ep the
                     * executed result (int_arg==1 records p-before-e). */
                    if ((cmd->sflags & SF_P) && cmd->int_arg == 1)
                        out_record(st, sb_str(&st->ps), sb_len(&st->ps), 1);
                    if (cmd->sflags & SF_E) {
                        strbuf_t res;
                        xb_init(&res, 128);
                        run_shell_into(st, sb_str(&st->ps), &res);
                        if (sb_len(&res) > 0 &&
                            sb_str(&res)[sb_len(&res)-1] == '\n')
                            sb_truncate(&res, sb_len(&res) - 1);
                        sb_reset(&st->ps);
                        sb_appendn(&st->ps, sb_str(&res), sb_len(&res));
                        sb_free(&res);
                    }
                    if ((cmd->sflags & SF_P) && cmd->int_arg != 1)
                        out_record(st, sb_str(&st->ps), sb_len(&st->ps), 1);
                    if (cmd->wfile && cmd->cmd == 's')
                        wfile_write(st, cmd, sb_str(&st->ps),
                                    sb_len(&st->ps), st->ps_had_delim);
                }
                break;
            }

            case 'y': {
                char *data = (char *)sb_str(&st->ps);
                size_t n = sb_len(&st->ps);
                for (size_t i = 0; i < n; i++)
                    data[i] = (char)cmd->ymap[(unsigned char)data[i]];
                break;
            }

            case 'b':
                cmd = cmd->jump;     /* NULL label -> end of script */
                if (!cmd) goto cycle_end;
                break;

            case 't':
                if (st->tflag) {
                    st->tflag = 0;
                    cmd = cmd->jump;
                    if (!cmd) goto cycle_end;
                }
                break;

            case 'T':
                if (!st->tflag) {
                    cmd = cmd->jump;
                    if (!cmd) goto cycle_end;
                } else {
                    st->tflag = 0;
                }
                break;

            default:
                break;
            }
            cmd = cmd->next;
        }

    cycle_end:
        if (cycle_print && !O.quiet)
            out_record(st, sb_str(&st->ps), sb_len(&st->ps), st->ps_had_delim);
        flush_appends(st);
        if (O.unbuffered)
            fflush(st->out);
        if (st->quit_code >= 0)
            return;
        if (!next_record(st))
            return;
        continue;

    cycle_end_final:
        if (cycle_print && !O.quiet)
            out_record(st, sb_str(&st->ps), sb_len(&st->ps), st->ps_had_delim);
        flush_appends(st);
        return;
    }

stream_end:
    flush_appends(st);
}

/* ------------------------------------------------------------------ */
/* In-place backup name resolution                                    */
/* ------------------------------------------------------------------ */

static char *backup_name(const char *fname, const char *suffix)
{
    strbuf_t sb;
    xb_init(&sb, strlen(fname) + strlen(suffix) + 1);
    if (strchr(suffix, '*')) {
        for (const char *q = suffix; *q; q++) {
            if (*q == '*')
                sb_append(&sb, fname);
            else
                sb_appendc(&sb, *q);
        }
    } else {
        sb_append(&sb, fname);
        sb_append(&sb, suffix);
    }
    char *out = strdup(sb_str(&sb));
    sb_free(&sb);
    return out;
}

/* ------------------------------------------------------------------ */
/* Usage / help / version                                             */
/* ------------------------------------------------------------------ */

static void print_usage_body(FILE *f)
{
    fputs(
"Usage: sed [OPTION]... {script-only-if-no-other-script} [input-file]...\n"
"\n"
"  -n, --quiet, --silent\n"
"                 suppress automatic printing of pattern space\n"
"      --debug\n"
"                 annotate program execution\n"
"  -e script, --expression=script\n"
"                 add the script to the commands to be executed\n"
"  -f script-file, --file=script-file\n"
"                 add the contents of script-file to the commands to be executed\n"
"  --follow-symlinks\n"
"                 follow symlinks when processing in place\n"
"  -i[SUFFIX], --in-place[=SUFFIX]\n"
"                 edit files in place (makes backup if SUFFIX supplied)\n"
"  -l N, --line-length=N\n"
"                 specify the desired line-wrap length for the 'l' command\n"
"  --posix\n"
"                 disable all GNU extensions.\n"
"  -E, -r, --regexp-extended\n"
"                 use extended regular expressions in the script\n"
"  -s, --separate\n"
"                 consider files as separate rather than as a single\n"
"                 continuous long stream.\n"
"      --sandbox\n"
"                 operate in sandbox mode (disable e/r/w commands).\n"
"  -u, --unbuffered\n"
"                 load minimal amounts of data from the input files and flush\n"
"                 the output buffers more often\n"
"  -z, --null-data\n"
"                 separate lines by NUL characters\n"
"      --help     display this help and exit\n"
"      --version  output version information and exit\n"
"\n"
"If no -e, --expression, -f, or --file option is given, then the first\n"
"non-option argument is taken as the sed script to interpret.  All\n"
"remaining arguments are names of input files; if no input files are\n"
"specified, then the standard input is read.\n"
"\n", f);
}

/* ------------------------------------------------------------------ */
/* Main applet entry                                                  */
/* ------------------------------------------------------------------ */

int applet_sed(int argc, char **argv)
{
    /* Reset global state: the applet runs in-process and may be invoked
     * repeatedly by the same shell. */
    memset(&O, 0, sizeof(O));
    O.line_len = 70;
    /* COLS sets the default l wrap width, minus one to avoid tty
     * line-wraps (GNU).  -l / --line-length still override it.
     * POSIXLY_CORRECT deliberately does NOT enable --posix mode (the
     * testsuite asserts GNU s-flags keep working under it). */
    {
        const char *cols = getenv("COLS");
        if (cols && *cols) {
            long c = strtol(cols, NULL, 10);
            if (c > 1)
                O.line_len = c - 1;
        }
    }
    g_exec_re = NULL;
    if (getenv("POSIXLY_CORRECT"))
        O.posix_env = 1;

    strbuf_t script;
    xb_init(&script, 256);
    sed_seg_t segs[128];
    int nsegs = 0;
    int expr_count = 0;
    int have_script = 0;

    sed_cmd_t *head = NULL;
    exec_t *st = NULL;
    parser_t ps;
    memset(&ps, 0, sizeof(ps));

    g_nrstreams = 0;
    g_nwstreams = 0;

    int jc = setjmp(sed_jmp);
    if (jc != 0) {
        /* error unwinding: free everything and return GNU's exit code */
        close_streams();
        free_cmds(head);
        sb_free(&script);
        if (st) {
            sb_free(&st->ps);
            sb_free(&st->hs);
            free(st->peek);
            free(st);
        }
        g_exec_re = NULL;
        return jc;
    }

#define ADD_SEG(isf, name) do {                                       \
        if (nsegs < 128) {                                            \
            segs[nsegs].is_file = (isf);                              \
            segs[nsegs].expr_index = (isf) ? 0 : ++expr_count;        \
            segs[nsegs].fname = (name);                               \
            segs[nsegs].start = sb_len(&script);                      \
            nsegs++;                                                  \
        }                                                             \
    } while (0)

    /* ---------- option parsing (GNU style) ---------- */
    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0')
            break;                          /* operand ("-" = stdin) */
        if (strcmp(arg, "--") == 0) { i++; break; }

        if (arg[1] == '-') {
            /* long options */
            const char *name = arg + 2;
            const char *val = strchr(name, '=');
            size_t nlen = val ? (size_t)(val - name) : strlen(name);
            if (val) val++;
#define LOPT(s) (strlen(s) == nlen && strncmp(name, s, nlen) == 0)
            if (LOPT("quiet") || LOPT("silent")) { O.quiet = 1; continue; }
            if (LOPT("posix"))            { O.posix = 1; continue; }
            if (LOPT("regexp-extended"))  { O.ere = 1; continue; }
            if (LOPT("separate"))         { O.separate = 1; continue; }
            if (LOPT("null-data"))        { O.nulldata = 1; continue; }
            if (LOPT("unbuffered"))       { O.unbuffered = 1; continue; }
            if (LOPT("sandbox"))          { O.sandbox = 1; continue; }
            if (LOPT("debug"))            { O.debug = 1; continue; }
            if (LOPT("follow-symlinks"))  { O.follow_syms = 1; continue; }
            if (LOPT("in-place")) {
                O.inplace = 1;
                O.separate = 1;
                O.in_suffix = val ? strdup(val) : NULL;
                continue;
            }
            if (LOPT("line-length")) {
                if (!val) sed_panic(1, "option '--line-length' requires an argument");
                O.line_len = strtol(val, NULL, 10);
                continue;
            }
            if (LOPT("expression")) {
                if (!val) {
                    if (++i >= argc)
                        sed_panic(1, "option '--expression' requires an argument");
                    val = argv[i];
                }
                if (sb_len(&script) > 0) sb_appendc(&script, '\n');
                ADD_SEG(0, NULL);
                sb_append(&script, val);
                have_script = 1;
                continue;
            }
            if (LOPT("file")) {
                if (!val) {
                    if (++i >= argc)
                        sed_panic(1, "option '--file' requires an argument");
                    val = argv[i];
                }
                goto read_script_file;
            }
            if (LOPT("help")) {
                print_usage_body(stdout);
                fputs("E-mail bug reports to: <bug-sed@gnu.org>.\n", stdout);
                sb_free(&script);
                /* GNU exits 4 ("panic") when even --help can't be
                 * written, e.g. redirected to /dev/full */
                if (fflush(stdout) != 0 || ferror(stdout)) {
                    clearerr(stdout);
                    return 4;
                }
                return 0;
            }
            if (LOPT("version")) {
                fputs(SED_VERSION_STR, stdout);
                sb_free(&script);
                if (fflush(stdout) != 0 || ferror(stdout)) {
                    clearerr(stdout);
                    return 4;
                }
                return 0;
            }
            fprintf(stderr, "sed: unrecognized option '--%.*s'\n",
                    (int)nlen, name);
            print_usage_body(stderr);
            sb_free(&script);
            return 1;
#undef LOPT
        read_script_file:
            {
                FILE *sfp = strcmp(val, "-") == 0 ? stdin : fopen(val, "r");
                if (!sfp)
                    sed_panic(4, "couldn't open file %s: %s", val,
                              strerror(errno));
                if (sb_len(&script) > 0) sb_appendc(&script, '\n');
                ADD_SEG(1, val);
                size_t seg_from = sb_len(&script);
                char rbuf[4096];
                size_t nr;
                while ((nr = fread(rbuf, 1, sizeof(rbuf), sfp)) > 0)
                    sb_appendn(&script, rbuf, nr);
                if (sfp != stdin) fclose(sfp);
                strip_crlf_from(&script, seg_from);
                have_script = 1;
                continue;
            }
        }

        /* short options, possibly clustered */
        /* Sentinel, compared by pointer identity below. Its CONTENTS never
         * matter; only that no character of `arg` shares its address. */
        static const char opt_consumed_rest[] = "";

        /* An option that takes an argument swallows the rest of this argv
         * element, so the cluster scan must stop. It said so by pointing `f` at
         * a literal "x" and testing `*f == 'x' && f != arg + 1 && f[-1] == 0`
         * -- which reads the byte BEFORE a string literal, out of bounds, on
         * every such option (cppcheck: negativeIndex). Nothing needed the read:
         * sed has no -x, so the test could only ever be reached with `f` on the
         * sentinel. A named sentinel compared by IDENTITY says the same thing
         * and cannot be confused with an option letter. */
        for (const char *f = arg + 1; *f; f++) {
            switch (*f) {
            case 'n': O.quiet = 1; break;
            case 'E': case 'r': O.ere = 1; break;
            case 's': O.separate = 1; break;
            case 'z': O.nulldata = 1; break;
            case 'u': O.unbuffered = 1; break;
            case 'e': {
                const char *val = f[1] ? f + 1 : NULL;
                if (!val) {
                    if (++i >= argc)
                        sed_panic(1, "option requires an argument -- 'e'");
                    val = argv[i];
                }
                if (sb_len(&script) > 0) sb_appendc(&script, '\n');
                ADD_SEG(0, NULL);
                sb_append(&script, val);
                have_script = 1;
                f = opt_consumed_rest;
                break;
            }
            case 'f': {
                const char *val = f[1] ? f + 1 : NULL;
                if (!val) {
                    if (++i >= argc)
                        sed_panic(1, "option requires an argument -- 'f'");
                    val = argv[i];
                }
                FILE *sfp = strcmp(val, "-") == 0 ? stdin : fopen(val, "r");
                if (!sfp)
                    sed_panic(4, "couldn't open file %s: %s", val,
                              strerror(errno));
                if (sb_len(&script) > 0) sb_appendc(&script, '\n');
                ADD_SEG(1, val);
                size_t seg_from = sb_len(&script);
                char rbuf[4096];
                size_t nr;
                while ((nr = fread(rbuf, 1, sizeof(rbuf), sfp)) > 0)
                    sb_appendn(&script, rbuf, nr);
                if (sfp != stdin) fclose(sfp);
                strip_crlf_from(&script, seg_from);
                have_script = 1;
                f = opt_consumed_rest;
                break;
            }
            case 'i': {
                O.inplace = 1;
                O.separate = 1;
                if (f[1]) {
                    free(O.in_suffix);
                    O.in_suffix = strdup(f + 1);
                    f = opt_consumed_rest;
                }
                break;
            }
            case 'l': {
                const char *val = f[1] ? f + 1 : NULL;
                if (!val) {
                    if (++i >= argc)
                        sed_panic(1, "option requires an argument -- 'l'");
                    val = argv[i];
                }
                O.line_len = strtol(val, NULL, 10);
                f = opt_consumed_rest;
                break;
            }
            default:
                fprintf(stderr, "sed: invalid option -- '%c'\n", *f);
                print_usage_body(stderr);
                sb_free(&script);
                free(O.in_suffix);
                return 1;
            }
            if (f == opt_consumed_rest) break;
        }
    }

    /* First operand is the script if none was given via -e/-f */
    if (!have_script) {
        if (i >= argc) {
            print_usage_body(stderr);
            sb_free(&script);
            free(O.in_suffix);
            return 1;
        }
        ADD_SEG(0, NULL);
        sb_append(&script, argv[i]);
        i++;
    }
#undef ADD_SEG

    REC_DELIM = O.nulldata ? '\0' : '\n';

    if (O.unbuffered)
        setvbuf(stdout, NULL, _IONBF, 0);
    else if (!isatty(STDOUT_FILENO)) {
        static char sed_out_buf[131072];
        setvbuf(stdout, sed_out_buf, _IOFBF, sizeof(sed_out_buf));
    }

    /* ---------- compile ---------- */
    ps.src = sb_str(&script);
    ps.p   = ps.src;
    ps.segs = segs;
    ps.nsegs = nsegs;
    head = parse_script(&ps, NULL);

    /* ---------- execute ---------- */
    int nfiles = argc - i;
    char **files = argv + i;
    char *dash = (char *)"-";
    if (nfiles == 0) {
        files = &dash;
        nfiles = 1;
    }

    if (O.inplace && argc - i == 0)
        sed_panic(1, "no input files while in-place editing");

    st = calloc(1, sizeof(*st));
    if (!st) sed_panic(4, "out of memory");
    xb_init(&st->ps, 256);
    xb_init(&st->hs, 8);
    st->hs_had_delim = 1;   /* the empty initial hold space ends a line */
    st->quit_code = -1;

    /* $ requires one record of lookahead; avoid it otherwise so we never
     * read more input than we consume (see -u / shared descriptors). */
    for (sed_cmd_t *c = head; c; c = c->next)
        if (c->a1.type == ADDR_LAST || (c->has_a2 && c->a2.type == ADDR_LAST))
            st->need_lookahead = 1;

    int exit_code = 0;

    if (!O.separate && !O.inplace) {
        /* one continuous stream */
        st->files = files;
        st->nfiles = nfiles;
        st->cur = 0;
        st->cur_name = "-";
        st->out = stdout;
        run_stream(head, st, &ps);
        /* a pending delimiter at stream end is DISCARDED: the missing
         * final newline of the input is preserved in the output */
        st->pending_nl = 0;
        if (st->quit_code > 0)
            exit_code = st->quit_code;
        if (st->input_open_failed && exit_code == 0)
            exit_code = 2;
    } else {
        for (int fi = 0; fi < nfiles; fi++) {
            char *one[1];
            char realbuf[PATH_MAX];
            const char *fname = files[fi];

            if (O.inplace && strcmp(fname, "-") != 0 && O.follow_syms) {
                if (realpath(fname, realbuf))
                    fname = realbuf;
            }
            one[0] = (char *)fname;

            /* reset per-file state (hold space too: GNU -i/-s does not
             * carry it across files -- inplace-hold expects empty) */
            for (sed_cmd_t *c = head; c; c = c->next) {
                c->active = 0;
                c->end_line = 0;
            }
            sb_reset(&st->hs);
            st->files = one;
            st->nfiles = 1;
            st->cur = 0;
            st->fp = NULL;
            st->have_peek = 0;
            st->linenum = 0;
            st->last_line = 0;
            st->naq = 0;
            st->pending_nl = 0;
            if (st->quit_code >= 0)
                break;

            char tmp_path[PATH_MAX];
            FILE *out_fp = stdout;
            if (O.inplace) {
                if (strcmp(fname, "-") == 0) {
                    /* GNU treats a literal "-" operand as a FILE named "-"
                     * for in-place editing */
                }
                struct stat stt;
                if (stat(fname, &stt) != 0 || !S_ISREG(stt.st_mode)) {
                    if (stat(fname, &stt) != 0) {
                        fprintf(stderr, "sed: can't read %s: %s\n", fname,
                                strerror(errno));
                        st->input_open_failed = 1;
                        continue;
                    }
                }
                char dir[PATH_MAX];
                path_dirname(fname, dir);
                int r = snprintf(tmp_path, sizeof(tmp_path), "%s/sedXXXXXX",
                                 dir);
                if (r < 0 || (size_t)r >= sizeof(tmp_path))
                    sed_panic(4, "couldn't open temporary file: name too long");
                int fd = mkstemp(tmp_path);
                if (fd < 0)
                    sed_panic(4, "couldn't open temporary file %s: %s",
                              tmp_path, strerror(errno));
                out_fp = fdopen(fd, "w");
                if (!out_fp) {
                    close(fd);
                    unlink(tmp_path);
                    sed_panic(4, "couldn't open temporary file %s: %s",
                              tmp_path, strerror(errno));
                }
            }

            st->out = out_fp;
            /* Run; on script runtime errors the longjmp handler cannot
             * clean the temp file, so do a nested setjmp here. */
            jmp_buf saved;
            memcpy(&saved, &sed_jmp, sizeof(jmp_buf));
            int rc = setjmp(sed_jmp);
            if (rc != 0) {
                if (O.inplace) {
                    fclose(out_fp);
                    unlink(tmp_path);
                }
                memcpy(&sed_jmp, &saved, sizeof(jmp_buf));
                longjmp(sed_jmp, rc);
            }
            run_stream(head, st, &ps);
            st->pending_nl = 0;   /* preserve missing final newline */
            memcpy(&sed_jmp, &saved, sizeof(jmp_buf));

            if (O.inplace) {
                fflush(out_fp);
                fclose(out_fp);
                st->out = stdout;
                struct stat stt;
                if (stat(fname, &stt) == 0)
                    chmod(tmp_path, stt.st_mode);
                if (O.in_suffix && O.in_suffix[0]) {
                    char *bak = backup_name(fname, O.in_suffix);
                    if (bak && strcmp(bak, fname) != 0) {
                        if (rename(fname, bak) != 0) {
                            fprintf(stderr,
                                    "sed: cannot rename %s to %s: %s\n",
                                    fname, bak, strerror(errno));
                            free(bak);
                            unlink(tmp_path);
                            longjmp(sed_jmp, 4);
                        }
                    }
                    free(bak);
                }
                if (rename(tmp_path, fname) != 0) {
                    fprintf(stderr, "sed: cannot rename %s to %s: %s\n",
                            tmp_path, fname, strerror(errno));
                    unlink(tmp_path);
                    longjmp(sed_jmp, 4);
                }
            }
        }
        if (st->quit_code > 0)
            exit_code = st->quit_code;
        if (st->input_open_failed && exit_code == 0)
            exit_code = 2;
    }

    if (fflush(stdout) != 0 || ferror(stdout)) {
        /* write error on the output stream (e.g. /dev/full): GNU sed
         * reports it and exits 4 */
        fprintf(stderr, "sed: couldn't write: %s\n", strerror(errno));
        clearerr(stdout);
        if (exit_code == 0)
            exit_code = 4;
    }

    /* ---------- cleanup ---------- */
    close_streams();
    sb_free(&st->ps);
    sb_free(&st->hs);
    free(st->peek);
    free(st);
    free_cmds(head);
    sb_free(&script);
    free(O.in_suffix);
    g_exec_re = NULL;
    return exit_code;
}
