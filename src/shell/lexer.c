/* lexer.c — shell lexer: tokenize input into shell tokens */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lexer.h"
#include "../util/arena.h"
#include "../util/charclass.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Word-terminator lookup table (LUT)
 *
 * word_stop[c] == 1 for every character that ends a word token.
 * Using a table lookup avoids 11 comparisons on every character in the
 * hot word-scanning loop.
 *
 * EOF is represented as (unsigned char)(-1) == 0xFF; entry [0xFF] = 1
 * ensures a single `word_stop[(unsigned char)c]` covers all cases.
 * ------------------------------------------------------------------------- */
static const uint8_t word_stop[256] = {
    ['\n']  = 1, [' ']  = 1, ['\t'] = 1,
    [';']   = 1, ['&']  = 1, ['|']  = 1,
    ['<']   = 1, ['>']  = 1, ['(']  = 1,  [')'] = 1,
    ['#']   = 1,
    [0xFF]  = 1,  /* (unsigned char)EOF — terminates word scan */
};

/* -------------------------------------------------------------------------
 * Reserved word table
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *word;
    tok_type_t  type;
} reserved_word_t;

static const reserved_word_t reserved_words[] = {
    { "if",       TOK_IF       },
    { "then",     TOK_THEN     },
    { "else",     TOK_ELSE     },
    { "elif",     TOK_ELIF     },
    { "fi",       TOK_FI       },
    { "while",    TOK_WHILE    },
    { "until",    TOK_UNTIL    },
    { "do",       TOK_DO       },
    { "done",     TOK_DONE     },
    { "for",      TOK_FOR      },
    { "in",       TOK_IN       },
    { "case",     TOK_CASE     },
    { "esac",     TOK_ESAC     },
    { "function", TOK_FUNCTION },
    { "!",        TOK_BANG     },
    { "{",        TOK_LBRACE   },
    { "}",        TOK_RBRACE   },
    { NULL,       TOK_EOF      },
};

/* -------------------------------------------------------------------------
 * Init / free
 * ------------------------------------------------------------------------- */

void lexer_init_str(lexer_t *l, const char *input, arena_t *a)
{
    memset(l, 0, sizeof(*l));
    l->input      = input;
    l->pos        = 0;
    l->fp         = NULL;
    l->lineno     = 1;
    l->arena      = a;
    l->has_peek   = 0;
    l->heredocs   = NULL;
    l->wordbuf_cap = 256;
    l->wordbuf     = malloc(l->wordbuf_cap);
    if (!l->wordbuf) {
        perror("matchbox: lexer_init_str: malloc");
        abort();
    }
    l->wordbuf_len = 0;
    l->wordbuf[0]  = '\0';
    l->has_pushback = 0;
}

void lexer_init_fp(lexer_t *l, FILE *fp, arena_t *a)
{
    memset(l, 0, sizeof(*l));
    l->input      = NULL;
    l->pos        = 0;
    l->fp         = fp;
    l->lineno     = 1;
    l->arena      = a;
    l->has_peek   = 0;
    l->heredocs   = NULL;
    l->wordbuf_cap = 256;
    l->wordbuf     = malloc(l->wordbuf_cap);
    if (!l->wordbuf) {
        perror("matchbox: lexer_init_fp: malloc");
        abort();
    }
    l->wordbuf_len = 0;
    l->wordbuf[0]  = '\0';
    l->has_pushback = 0;
}

void lexer_free(lexer_t *l)
{
    free(l->wordbuf);
    l->wordbuf     = NULL;
    l->wordbuf_len = 0;
    l->wordbuf_cap = 0;
}

/* -------------------------------------------------------------------------
 * Low-level character I/O
 * ------------------------------------------------------------------------- */

static int lexer_getc(lexer_t *l)
{
    if (l->has_pushback) {
        l->has_pushback = 0;
        return l->pushback;
    }
    if (l->input) {
        unsigned char c = (unsigned char)l->input[l->pos];
        if (c == '\0')
            return EOF;
        l->pos++;
        return (int)c;
    }
    return fgetc(l->fp);
}

static void lexer_ungetc(lexer_t *l, int c)
{
    /* Only supports a single character of pushback */
    l->pushback     = c;
    l->has_pushback = 1;
}

/* -------------------------------------------------------------------------
 * Word buffer helpers
 * ------------------------------------------------------------------------- */

static void wordbuf_reset(lexer_t *l)
{
    l->wordbuf_len = 0;
    l->wordbuf[0]  = '\0';
}

static void wordbuf_append(lexer_t *l, char c)
{
    /* Ensure space for char + NUL */
    if (l->wordbuf_len + 2 > l->wordbuf_cap) {
        size_t newcap = l->wordbuf_cap * 2;
        char  *newbuf = realloc(l->wordbuf, newcap);
        if (!newbuf) {
            perror("matchbox: lexer wordbuf realloc");
            abort();
        }
        l->wordbuf     = newbuf;
        l->wordbuf_cap = newcap;
    }
    l->wordbuf[l->wordbuf_len++] = c;
    l->wordbuf[l->wordbuf_len]   = '\0';
}

static void wordbuf_appends(lexer_t *l, const char *s)
{
    for (; *s; s++)
        wordbuf_append(l, *s);
}

/* -------------------------------------------------------------------------
 * Reserved word check
 * ------------------------------------------------------------------------- */

static tok_type_t check_reserved(const char *word)
{
    for (int i = 0; reserved_words[i].word; i++) {
        if (strcmp(reserved_words[i].word, word) == 0)
            return reserved_words[i].type;
    }
    return TOK_WORD;
}

/* -------------------------------------------------------------------------
 * Heredoc reading
 * ------------------------------------------------------------------------- */

/*
 * Read a heredoc body from the input.  Reads line by line until a line
 * that matches delim (with optional leading tabs stripped for strip_tabs).
 * Appends the body to the wordbuf, then returns an arena-duplicated string.
 */
static char *read_heredoc_body(lexer_t *l, const char *delim, int strip_tabs)
{
    wordbuf_reset(l);

    for (;;) {
        /* Read one line into a temporary strbuf */
        char linebuf[4096];
        size_t linelen = 0;

        for (;;) {
            int c = lexer_getc(l);
            if (c == EOF) {
                /* Unterminated heredoc — return what we have */
                goto done;
            }
            if (c == '\n') {
                l->lineno++;
                break;
            }
            if (linelen + 1 < sizeof(linebuf) - 1)
                linebuf[linelen++] = (char)c;
        }
        linebuf[linelen] = '\0';

        /* Check for delimiter (strip leading tabs if requested) */
        const char *check = linebuf;
        if (strip_tabs) {
            while (*check == '\t')
                check++;
        }
        if (strcmp(check, delim) == 0)
            goto done;

        /* Append line + newline to wordbuf */
        wordbuf_appends(l, linebuf);
        wordbuf_append(l, '\n');
    }

done:
    return arena_strdup(l->arena, l->wordbuf);
}

/* -------------------------------------------------------------------------
 * Quote and substitution handling inside word scanning
 * ------------------------------------------------------------------------- */

/*
 * Collect characters inside $(...) — track nesting depth.
 * Called after the opening '(' has been consumed.
 * Appends everything including the final ')' to wordbuf.
 */
static void scan_cmd_subst(lexer_t *l)
{
    int depth = 1;
    wordbuf_append(l, '(');
    while (depth > 0) {
        int c = lexer_getc(l);
        if (c == EOF) break;
        if (c == '(') {
            depth++;
            wordbuf_append(l, (char)c);
        } else if (c == ')') {
            depth--;
            wordbuf_append(l, ')');
        } else if (c == '\'') {
            wordbuf_append(l, '\'');
            /* pass through single-quoted block literally */
            for (;;) {
                int q = lexer_getc(l);
                if (q == EOF || q == '\'') {
                    wordbuf_append(l, '\'');
                    break;
                }
                wordbuf_append(l, (char)q);
            }
        } else if (c == '"') {
            wordbuf_append(l, '"');
            /* pass through double-quoted block */
            for (;;) {
                int q = lexer_getc(l);
                if (q == EOF) break;
                if (q == '"') {
                    wordbuf_append(l, '"');
                    break;
                }
                if (q == '\\') {
                    int e = lexer_getc(l);
                    wordbuf_append(l, '\\');
                    if (e != EOF) wordbuf_append(l, (char)e);
                } else {
                    wordbuf_append(l, (char)q);
                }
            }
        } else if (c == '\n') {
            l->lineno++;
            wordbuf_append(l, '\n');
        } else {
            wordbuf_append(l, (char)c);
        }
    }
}

/*
 * Collect characters inside ${...} — track nesting depth.
 * Called after the opening '{' has been consumed.
 * Appends everything including the final '}' to wordbuf.
 */
static void scan_param_expand(lexer_t *l)
{
    int depth = 1;
    wordbuf_append(l, '{');
    while (depth > 0) {
        int c = lexer_getc(l);
        if (c == EOF) break;
        if (c == '{') {
            depth++;
            wordbuf_append(l, (char)c);
        } else if (c == '}') {
            depth--;
            wordbuf_append(l, '}');
        } else if (c == '\n') {
            l->lineno++;
            wordbuf_append(l, '\n');
        } else {
            wordbuf_append(l, (char)c);
        }
    }
}

/*
 * Collect characters inside $((...)) arithmetic expansion.
 * Called after "((" have been consumed.
 * Appends everything including "))" to wordbuf.
 */
static void scan_arith(lexer_t *l)
{
    wordbuf_appends(l, "((");
    int depth = 2; /* track matching parens */
    while (depth > 0) {
        int c = lexer_getc(l);
        if (c == EOF) break;
        if (c == '(') {
            depth++;
            wordbuf_append(l, (char)c);
        } else if (c == ')') {
            depth--;
            wordbuf_append(l, ')');
        } else if (c == '\n') {
            l->lineno++;
            wordbuf_append(l, '\n');
        } else {
            wordbuf_append(l, (char)c);
        }
    }
}

/*
 * Scan single-quoted string (everything until closing ').
 * Called after the opening '\'' has been consumed.
 */
static void scan_single_quote(lexer_t *l)
{
    wordbuf_append(l, '\'');
    for (;;) {
        int c = lexer_getc(l);
        if (c == EOF || c == '\'') {
            wordbuf_append(l, '\'');
            break;
        }
        if (c == '\n') l->lineno++;
        wordbuf_append(l, (char)c);
    }
}

/*
 * Scan double-quoted string.  $, `, \ are still active.
 * Called after the opening '"' has been consumed.
 */
static void scan_double_quote(lexer_t *l)
{
    wordbuf_append(l, '"');
    for (;;) {
        int c = lexer_getc(l);
        if (c == EOF || c == '"') {
            wordbuf_append(l, '"');
            break;
        }
        if (c == '\n') {
            l->lineno++;
            wordbuf_append(l, '\n');
            continue;
        }
        if (c == '\\') {
            int e = lexer_getc(l);
            wordbuf_append(l, '\\');
            if (e != EOF) {
                if (e == '\n') l->lineno++;
                wordbuf_append(l, (char)e);
            }
            continue;
        }
        if (c == '$') {
            int next = lexer_getc(l);
            if (next == '(') {
                /* Check for $(( */
                int next2 = lexer_getc(l);
                if (next2 == '(') {
                    wordbuf_append(l, '$');
                    scan_arith(l);
                } else {
                    lexer_ungetc(l, next2);
                    wordbuf_append(l, '$');
                    scan_cmd_subst(l);
                }
            } else if (next == '{') {
                wordbuf_append(l, '$');
                scan_param_expand(l);
            } else {
                wordbuf_append(l, '$');
                if (next != EOF) {
                    if (next == '\n') l->lineno++;
                    wordbuf_append(l, (char)next);
                }
            }
            continue;
        }
        if (c == '`') {
            wordbuf_append(l, '`');
            /* backtick command substitution */
            for (;;) {
                int q = lexer_getc(l);
                if (q == EOF) break;
                if (q == '`') {
                    wordbuf_append(l, '`');
                    break;
                }
                if (q == '\\') {
                    int e = lexer_getc(l);
                    wordbuf_append(l, '\\');
                    if (e != EOF) {
                        if (e == '\n') l->lineno++;
                        wordbuf_append(l, (char)e);
                    }
                } else {
                    if (q == '\n') l->lineno++;
                    wordbuf_append(l, (char)q);
                }
            }
            continue;
        }
        wordbuf_append(l, (char)c);
    }
}

/* -------------------------------------------------------------------------
 * Determine if a completed word token contains an assignment
 * i.e., NAME=VALUE where NAME matches [A-Za-z_][A-Za-z0-9_]*
 * Returns 1 if it looks like an assignment.
 * ------------------------------------------------------------------------- */
static int is_assignment(const char *s)
{
    if (!s || !*s)
        return 0;
    /* First char must be alpha or underscore */
    if (!is_alpha_underscore((unsigned char)*s))
        return 0;
    const char *p = s + 1;
    while (is_name_char((unsigned char)*p))
        p++;
    return *p == '=';
}

/* -------------------------------------------------------------------------
 * Token construction helpers
 * ------------------------------------------------------------------------- */

static token_t make_tok(lexer_t *l, tok_type_t type)
{
    token_t t;
    t.type   = type;
    t.text   = NULL;
    t.lineno = l->lineno;
    return t;
}

static token_t make_word_tok(lexer_t *l, int quoted, int has_unquoted_assign)
{
    token_t t;
    /* Determine type:
     * - Assignment if NAME= prefix was unquoted (value may be quoted)
     * - Reserved word only if entirely unquoted
     * - Otherwise a plain word */
    if (has_unquoted_assign && is_assignment(l->wordbuf)) {
        t.type = TOK_ASSIGN;
    } else if (!quoted) {
        tok_type_t rw = check_reserved(l->wordbuf);
        t.type = rw;
    } else {
        t.type = TOK_WORD;
    }
    t.text   = arena_strdup(l->arena, l->wordbuf);
    t.lineno = l->lineno;
    return t;
}

/* -------------------------------------------------------------------------
 * Main lexer: read and return the next raw token (no lookahead)
 * ------------------------------------------------------------------------- */

static token_t lexer_read(lexer_t *l)
{
    int c;

restart:
    /* Skip spaces and tabs (but not newlines) */
    do {
        c = lexer_getc(l);
    } while (c == ' ' || c == '\t');

    if (c == EOF)
        return make_tok(l, TOK_EOF);

    int start_lineno = l->lineno;

    /* Comment: skip to end of line */
    if (c == '#') {
        while (c != '\n' && c != EOF)
            c = lexer_getc(l);
        /* Fall through with c == '\n' or EOF */
        if (c == EOF)
            return make_tok(l, TOK_EOF);
        /* c == '\n': fall through to newline handling below */
    }

    if (c == '\n') {
        token_t t = make_tok(l, TOK_NEWLINE);
        l->lineno++;

        /* After a newline, drain any pending heredocs */
        if (l->heredocs) {
            heredoc_pending_t *hp = l->heredocs;
            while (hp) {
                heredoc_pending_t *next = hp->next;
                /* Read the heredoc body and store it in the redir_t via
                 * the body_out back-pointer set by the parser. */
                char *body = read_heredoc_body(l, hp->delim, hp->strip_tabs);
                if (hp->body_out)
                    *hp->body_out = body;
                hp = next;
            }
            l->heredocs = NULL;
        }
        return t;
    }

    /* Two-character operators */
    if (c == '&') {
        int next = lexer_getc(l);
        if (next == '&') {
            token_t t = make_tok(l, TOK_AND_AND);
            t.lineno = start_lineno;
            return t;
        }
        lexer_ungetc(l, next);
        token_t t = make_tok(l, TOK_AMP);
        t.lineno = start_lineno;
        return t;
    }

    if (c == '|') {
        int next = lexer_getc(l);
        if (next == '|') {
            token_t t = make_tok(l, TOK_OR_OR);
            t.lineno = start_lineno;
            return t;
        }
        lexer_ungetc(l, next);
        token_t t = make_tok(l, TOK_PIPE);
        t.lineno = start_lineno;
        return t;
    }

    if (c == ';') {
        int next = lexer_getc(l);
        if (next == ';') {
            token_t t = make_tok(l, TOK_DSEMI);
            t.lineno = start_lineno;
            return t;
        }
        lexer_ungetc(l, next);
        token_t t = make_tok(l, TOK_SEMI);
        t.lineno = start_lineno;
        return t;
    }

    if (c == '<') {
        int next = lexer_getc(l);
        if (next == '<') {
            int next2 = lexer_getc(l);
            if (next2 == '-') {
                token_t t = make_tok(l, TOK_DLESSDASH);
                t.lineno = start_lineno;
                return t;
            }
            lexer_ungetc(l, next2);
            token_t t = make_tok(l, TOK_DLESS);
            t.lineno = start_lineno;
            return t;
        }
        if (next == '&') {
            token_t t = make_tok(l, TOK_LESSAND);
            t.lineno = start_lineno;
            return t;
        }
        if (next == '>') {
            token_t t = make_tok(l, TOK_LESSGREAT);
            t.lineno = start_lineno;
            return t;
        }
        lexer_ungetc(l, next);
        token_t t = make_tok(l, TOK_LESS);
        t.lineno = start_lineno;
        return t;
    }

    if (c == '>') {
        int next = lexer_getc(l);
        if (next == '>') {
            token_t t = make_tok(l, TOK_DGREAT);
            t.lineno = start_lineno;
            return t;
        }
        if (next == '&') {
            token_t t = make_tok(l, TOK_GREATAND);
            t.lineno = start_lineno;
            return t;
        }
        if (next == '|') {
            token_t t = make_tok(l, TOK_CLOBBER);
            t.lineno = start_lineno;
            return t;
        }
        lexer_ungetc(l, next);
        token_t t = make_tok(l, TOK_GREAT);
        t.lineno = start_lineno;
        return t;
    }

    if (c == '(') {
        token_t t = make_tok(l, TOK_LPAREN);
        t.lineno = start_lineno;
        return t;
    }
    if (c == ')') {
        token_t t = make_tok(l, TOK_RPAREN);
        t.lineno = start_lineno;
        return t;
    }

    /* Word token: collect characters */
    wordbuf_reset(l);
    int quoted = 0;            /* 1 if any quoting was used */
    int has_unquoted_assign = 0; /* 1 if NAME= prefix was seen unquoted */

    for (;;) {
        if (c == '\'') {
            quoted = 1;
            scan_single_quote(l);
        } else if (c == '"') {
            quoted = 1;
            scan_double_quote(l);
        } else if (c == '\\') {
            int e = lexer_getc(l);
            if (e == '\n') {
                /* line continuation — skip */
                l->lineno++;
            } else if (e == EOF) {
                wordbuf_append(l, '\\');
            } else {
                wordbuf_append(l, '\\');
                wordbuf_append(l, (char)e);
                quoted = 1; /* backslash-quoted character */
            }
        } else if (c == '$') {
            int next = lexer_getc(l);
            if (next == '(') {
                int next2 = lexer_getc(l);
                if (next2 == '(') {
                    wordbuf_append(l, '$');
                    scan_arith(l);
                } else {
                    lexer_ungetc(l, next2);
                    wordbuf_append(l, '$');
                    scan_cmd_subst(l);
                }
            } else if (next == '{') {
                wordbuf_append(l, '$');
                scan_param_expand(l);
            } else {
                wordbuf_append(l, '$');
                if (next != EOF) {
                    wordbuf_append(l, (char)next);
                }
            }
        } else if (c == '`') {
            wordbuf_append(l, '`');
            for (;;) {
                int q = lexer_getc(l);
                if (q == EOF) break;
                if (q == '`') {
                    wordbuf_append(l, '`');
                    break;
                }
                if (q == '\\') {
                    int e = lexer_getc(l);
                    wordbuf_append(l, '\\');
                    if (e != EOF) {
                        if (e == '\n') l->lineno++;
                        wordbuf_append(l, (char)e);
                    }
                } else {
                    if (q == '\n') l->lineno++;
                    wordbuf_append(l, (char)q);
                }
            }
        } else {
            /* Check for word-terminating characters via LUT (one table lookup
             * vs. 11 comparisons; also covers EOF via 0xFF entry). */
            if (word_stop[(unsigned char)c]) {
                lexer_ungetc(l, c);
                break;
            }
            /* Detect NAME= in unquoted context for assignment recognition:
             * wordbuf currently holds the NAME part (before appending '=') */
            if (c == '=' && !quoted && !has_unquoted_assign &&
                l->wordbuf_len > 0 &&
                is_alpha_underscore((unsigned char)l->wordbuf[0])) {
                int all_name = 1;
                for (size_t ni = 1; ni < l->wordbuf_len; ni++) {
                    if (!is_name_char((unsigned char)l->wordbuf[ni])) {
                        all_name = 0; break;
                    }
                }
                if (all_name) has_unquoted_assign = 1;
            }
            wordbuf_append(l, (char)c);
        }

        c = lexer_getc(l);
        if (c == EOF) break;
    }

    if (l->wordbuf_len == 0) {
        /* Somehow ended up with an empty word; retry */
        goto restart;
    }

    token_t t = make_word_tok(l, quoted, has_unquoted_assign);
    t.lineno  = start_lineno;
    return t;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

token_t lexer_next(lexer_t *l)
{
    if (l->has_peek) {
        l->has_peek = 0;
        return l->peek;
    }
    return lexer_read(l);
}

token_t lexer_peek(lexer_t *l)
{
    if (!l->has_peek) {
        l->peek     = lexer_read(l);
        l->has_peek = 1;
    }
    return l->peek;
}

void lexer_consume(lexer_t *l)
{
    if (l->has_peek) {
        l->has_peek = 0;
    } else {
        /* Discard one token */
        lexer_read(l);
    }
}
