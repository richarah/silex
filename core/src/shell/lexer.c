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
 * There used to be a [0xFF] = 1 entry here, on the theory that EOF is
 * (unsigned char)(-1) == 0xFF so one table lookup could cover EOF too.
 *
 * That was an INFINITE LOOP. 0xFF is a perfectly real byte, and it indexes the
 * same slot as the EOF sentinel: the word loop saw a literal 0xFF, decided it
 * was a word terminator, ungetc'd it, and read the very same byte back on the
 * next iteration -- forever. A single stray high byte in a script hung the
 * shell. (Found by fuzz_shell_lexer, the first time it was ever built and run;
 * minimised to the one byte 0xFF.)
 *
 * EOF is an int -1 and is already handled explicitly at the bottom of the word
 * loop. The table only ever indexes genuine bytes, so it must not claim any
 * byte value is EOF.
 * ------------------------------------------------------------------------- */
static const uint8_t word_stop[256] = {
    ['\n']  = 1, [' ']  = 1, ['\t'] = 1,
    [';']   = 1, ['&']  = 1, ['|']  = 1,
    ['<']   = 1, ['>']  = 1, ['(']  = 1,  [')'] = 1,
    /* '#' is deliberately absent: a comment starts only where a new token
     * starts (POSIX 2.3 rule 9, handled in lexer_read). Mid-word '#' as in
     * `foo#bar` or `x=$a#$b` is a literal character. */
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
        perror("silex: lexer_init_str: malloc");
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
        perror("silex: lexer_init_fp: malloc");
        abort();
    }
    l->wordbuf_len = 0;
    l->wordbuf[0]  = '\0';
    l->has_pushback = 0;
}

void lexer_set_verbose(lexer_t *l, const int *flag)
{
    l->verbose = flag;
}

/* -v echoes the shell's input verbatim as it is consumed. Buffered per line
 * rather than written per character: stderr is unbuffered, so a naive fputc
 * per character costs one write(2) per byte of script. */
static void verbose_flush(lexer_t *l)
{
    if (l->vlen) {
        fwrite(l->vbuf, 1, l->vlen, stderr);
        fflush(stderr);
        l->vlen = 0;
    }
}

static void verbose_emit(lexer_t *l, int c)
{
    l->vbuf[l->vlen++] = (char)c;
    if (c == '\n' || l->vlen == sizeof(l->vbuf))
        verbose_flush(l);
}

void lexer_free(lexer_t *l)
{
    verbose_flush(l);
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
    int c;
    if (l->input) {
        c = (unsigned char)l->input[l->pos];
        if (c == '\0')
            return EOF;
        l->pos++;
    } else {
        c = fgetc(l->fp);
        if (c == EOF)
            return EOF;
    }
    if (l->verbose && *l->verbose)
        verbose_emit(l, c);
    return c;
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
            perror("silex: lexer wordbuf realloc");
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
static char *read_heredoc_body(lexer_t *l, const char *delim, int strip_tabs,
                               int no_expand)
{
    wordbuf_reset(l);

    /* In an EXPANDING here-doc a physical line ending in an unescaped backslash
     * is a line continuation: the next physical line is part of the same logical
     * line, so it must NOT be tested as the delimiter. (`jkl\<newline>FIN` keeps
     * FIN as body, not the terminator.) The `\<newline>` join and <<- tab strip
     * are done later at redirect time; here we only gate the delimiter test.
     * A quoted delimiter (no_expand) disables continuation -- backslash is then
     * literal. */
    int prev_continued = 0;

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

        /* Check for delimiter (strip leading tabs if requested) -- but not when
         * this line is the continuation of the previous one. */
        if (!prev_continued) {
            const char *check = linebuf;
            if (strip_tabs) {
                while (*check == '\t')
                    check++;
            }
            if (strcmp(check, delim) == 0)
                goto done;
        }

        /* Append line + newline to wordbuf */
        wordbuf_appends(l, linebuf);
        wordbuf_append(l, '\n');

        /* Does THIS line continue into the next? (odd trailing backslashes) */
        prev_continued = 0;
        if (!no_expand) {
            size_t bs = 0;
            while (bs < linelen && linebuf[linelen - 1 - bs] == '\\')
                bs++;
            if (bs & 1)
                prev_continued = 1;
        }
    }

done:
    return arena_strdup(l->arena, l->wordbuf);
}

/* -------------------------------------------------------------------------
 * Quote and substitution handling inside word scanning
 * ------------------------------------------------------------------------- */

/* Record that a construct ran off the end of the input. The first one wins:
 * an unterminated `"` inside an unterminated `$(` should name the innermost
 * thing that actually failed to close. */
static void lex_unterminated(lexer_t *l, const char *what)
{
    if (!l->error) l->error = what;
}

/*
 * Collect characters inside $(...) — track nesting depth.
 * Called after the opening '(' has been consumed.
 * Appends everything including the final ')' to wordbuf.
 */
static void scan_cmd_subst(lexer_t *l)
{
    int depth = 1;
    wordbuf_append(l, '(');
    /* Char before the current one, to spot a `#` in command position. Seeded
     * with '(' (the just-consumed `$(`), itself a command-word delimiter. */
    int prev = '(';
    /* `case ... esac` awareness (mirrors cmdsubst_body_end in expand.c): an
     * unprefixed case pattern's bare `)` must not close the substitution.
     * While inside case..esac, parens don't adjust depth at all. */
    int case_depth = 0;
    char kw[8]; int kwlen = 0; int at_cmdpos = 1, word_cmdpos = 0;
    while (depth > 0) {
        int c = lexer_getc(l);
        if (c == EOF) { lex_unterminated(l, "unexpected EOF in $( (expecting `)')"); break; }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            if (kwlen == 0) word_cmdpos = at_cmdpos;
            if (kwlen < 7) kw[kwlen++] = (char)c;
            else word_cmdpos = 0;
        } else {
            if (kwlen) {
                kw[kwlen] = '\0';
                if (word_cmdpos && strcmp(kw, "case") == 0) case_depth++;
                else if (word_cmdpos && strcmp(kw, "esac") == 0 && case_depth > 0)
                    case_depth--;
                kwlen = 0;
                at_cmdpos = 0;   /* the word we just saw fills the command slot */
            }
            if (c == ';' || c == '\n' || c == '&' || c == '|' || c == '(')
                at_cmdpos = 1;
            else if (c != ' ' && c != '\t')
                at_cmdpos = 0;
        }
        if (c == '#' &&
            (prev == ' ' || prev == '\t' || prev == '\n' || prev == '(' ||
             prev == ';' || prev == '&'  || prev == '|')) {
            /* Comment: everything to end of line is literal, so a stray ' " ( )
             * in it must not affect quote/paren tracking. `$( # subshell's PID
             * )` -- an apostrophe in a comment -- otherwise opened a runaway
             * single-quote scan that swallowed the closing ')'. Append the text
             * verbatim (it is re-lexed, and skipped again, when the substitution
             * runs); stop at the newline and let the loop handle it. */
            wordbuf_append(l, '#');
            for (;;) {
                int q = lexer_getc(l);
                if (q == EOF) break;
                if (q == '\n') { lexer_ungetc(l, q); break; }
                wordbuf_append(l, (char)q);
            }
            prev = '#';
            continue;
        }
        prev = c;
        if (c == '\\') {
            /* Backslash escapes the next character outside quotes: \( \) \' \"
             * are literals and must NOT affect paren depth or start a quoted
             * block. Pass both bytes through verbatim. */
            int e = lexer_getc(l);
            wordbuf_append(l, '\\');
            if (e != EOF) {
                if (e == '\n') l->lineno++;
                wordbuf_append(l, (char)e);
            }
        } else if (c == '(') {
            if (!case_depth) depth++;
            wordbuf_append(l, (char)c);
        } else if (c == ')') {
            if (!case_depth) depth--;
            else { wordbuf_append(l, ')'); continue; }
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
 * Collect characters inside `...` — the backquote form of command substitution.
 * Called after the opening '`' has been consumed.
 * Appends everything including the closing '`' to wordbuf.
 */
static void scan_backquote(lexer_t *l)
{
    wordbuf_append(l, '`');
    for (;;) {
        int q = lexer_getc(l);
        if (q == EOF) { lex_unterminated(l, "EOF in backquote substitution"); break; }
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
}

static void scan_arith(lexer_t *l);
static int is_special_param_char(int c);

/*
 * Collect characters inside ${...} — find the matching '}'.
 * Called after the opening '{' has been consumed.
 * Appends everything including the final '}' to wordbuf.
 */
static void scan_param_expand(lexer_t *l, int outer_dq)
{
    /* Finding the closing '}' is a SCAN, not a brace count. A bare `{` inside an
     * expansion is ordinary data in every POSIX shell -- `${x:+a{b}` ends at the
     * first `}` -- so only a nested `${` opens a level, and it is consumed by
     * recursing rather than by a counter. Counting bare braces made `${x:+a{b}`
     * an unterminated-expansion error, and only ever "worked" where the stray
     * braces happened to balance: modernish's `${x:+'...{...'\}}` survived
     * because the miscounted quoted `{` and the `\}` cancelled out.
     *
     * The other constructs that can hide a `}` are handed to the same scanners
     * the rest of the lexer uses: $(...), $((...)) and `...`. Without that,
     * `${x:+$(echo })}` ended the token inside the substitution.
     *
     * Quoting: a `\}` is literal; an INNER `"` opened here protects a `}`, while
     * the enclosing "..." (outer_dq) does not. A `'` quotes only outside double
     * quotes -- `"${x-'}"` has a literal `'` -- EXCEPT in the word of a pattern
     * operator (`#` `##` `%` `%%` `/`), where dash and bash agree that it quotes
     * even within "...", making `"${x%d'}"` an unterminated-quote error.
     *
     * Everything is kept verbatim in the token; expand.c does the actual quote
     * and backslash removal (with the same rule). */
    int in_squote = 0, inner_dq = 0;
    int squote_quotes = !outer_dq;  /* is `'` a quoting character here? */
    int in_header = 1;              /* still scanning ${name, before the operator */
    int at_start = 1;
    wordbuf_append(l, '{');
    for (;;) {
        int c = lexer_getc(l);
        if (c == EOF) { lex_unterminated(l, "unexpected EOF in ${ (missing `}')"); return; }
        if (in_squote) {
            /* Single quotes: everything literal (backslash included) until '. */
            wordbuf_append(l, (char)c);
            if (c == '\n') l->lineno++;
            else if (c == '\'') in_squote = 0;
            continue;
        }
        if (c == '\\') {
            /* Escapes the next byte; the `}` in `\}` does not close the word. */
            int n = lexer_getc(l);
            wordbuf_append(l, '\\');
            if (n == EOF) { lex_unterminated(l, "unexpected EOF in ${ (missing `}')"); return; }
            if (n == '\n') l->lineno++;
            wordbuf_append(l, (char)n);
            in_header = at_start = 0;
            continue;
        }
        if (in_header) {
            /* The parameter name: name characters, or a one-character special
             * param (`@ * # ? - $ ! 0`), which is only a name at the very front.
             * A leading `#` is the length PREFIX of `${#name}`, not an operator,
             * and does not make `'` quote (dash reads `"${#x'}"` as the bad name
             * `x'`). The first character that cannot continue the name IS the
             * operator, and decides -- so the `#` of `${x#pat}`, which is not at
             * the front, must not be mistaken for the special param `$#`. */
            if ((at_start && (c == '#' || is_special_param_char(c))) ||
                is_name_char((unsigned char)c)) {
                at_start = 0;
                wordbuf_append(l, (char)c);
                continue;
            }
            in_header = at_start = 0;
            if (c == ':') {
                /* `:-` `:+` `:=` `:?` (and the substring forms): the operator is
                 * the next character, and none of them is a pattern operator. */
                wordbuf_append(l, ':');
                continue;
            }
            if (c == '#' || c == '%' || c == '/') squote_quotes = 1;
            /* fall through: the operator itself is ordinary data */
        }
        if (c == '\'' && squote_quotes && !inner_dq) {
            in_squote = 1;
            wordbuf_append(l, '\'');
            continue;
        }
        if (c == '"') {
            inner_dq = !inner_dq;
            wordbuf_append(l, '"');
            continue;
        }
        if (c == '$') {
            int n = lexer_getc(l);
            if (n == '{') {
                wordbuf_append(l, '$');
                scan_param_expand(l, outer_dq || inner_dq);
                continue;
            }
            if (n == '(') {
                int n2 = lexer_getc(l);
                wordbuf_append(l, '$');
                if (n2 == '(') {
                    scan_arith(l);
                } else {
                    lexer_ungetc(l, n2);
                    scan_cmd_subst(l);
                }
                continue;
            }
            /* Not an expansion: the `$` is literal and the next character keeps
             * its own meaning (it may well be the closing `}`). */
            wordbuf_append(l, '$');
            if (n != EOF) lexer_ungetc(l, n);
            continue;
        }
        if (c == '`') {
            scan_backquote(l);
            continue;
        }
        if (c == '}' && !inner_dq) {
            wordbuf_append(l, '}');
            return;
        }
        if (c == '\n') l->lineno++;
        wordbuf_append(l, (char)c);
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
        if (c == EOF) { lex_unterminated(l, "unexpected EOF in $(( (missing `))')"); break; }
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
            if (c == EOF) lex_unterminated(l, "unterminated quoted string");
            wordbuf_append(l, '\'');
            break;
        }
        if (c == '\n') l->lineno++;
        wordbuf_append(l, (char)c);
    }
}

/* The one-character parameter names: `$?`, `$@`, `$*`, `$#`, `$-`, `$$`,
 * `$!`, `$0`. Anything else after a `$` (that is not a name character, `{`
 * or `(`) leaves the `$` literal. */
static int is_special_param_char(int c)
{
    return c == '@' || c == '*' || c == '#' || c == '?' ||
           c == '-' || c == '$' || c == '!' || c == '0';
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
            if (c == EOF) lex_unterminated(l, "unterminated quoted string");
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
                scan_param_expand(l, 1);
            } else if (next == '"' || next == '`' || next == '\\') {
                /* A `$` that is not followed by a name, `{` or `(` is a
                 * literal dollar -- and the character after it keeps its own
                 * meaning. Swallowing it here consumed the CLOSING quote of
                 * `"$"`, so the quote scan ran on to end of input and the rest
                 * of the line became part of the word (`printf "%s" "$"; echo`
                 * passed `; echo` to printf). Push it back for the loop. */
                wordbuf_append(l, '$');
                lexer_ungetc(l, next);
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
            scan_backquote(l);
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

static token_t lexer_read_raw(lexer_t *l)
{
    int c;

restart:
    /* Skip spaces and tabs (but not newlines). Whether any were skipped is
     * recorded for the token: the IO_NUMBER rule below needs to know that
     * `6>` and `6 >` are different things. */
    do {
        c = lexer_getc(l);
        if (c == ' ' || c == '\t') l->blank_before = 1;
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
                char *body = read_heredoc_body(l, hp->delim, hp->strip_tabs,
                                               hp->no_expand);
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
                scan_param_expand(l, 0);
            } else if (next == '\'' || next == '"') {
                /* `$'...'` / `$"..."` are not POSIX (they are bash/ksh ANSI-C and
                 * locale quoting). POSIX treats the `$` as a literal dollar and
                 * the quote as opening an ordinary quoted string, so `$'a\40b'`
                 * is the word `$a\40b` -- which is exactly what lets modernish's
                 * CESCQUOT capability probe fail gracefully instead of parse-
                 * erroring. Push the quote back so the main loop pairs it: the
                 * old code appended the OPENING quote as a literal, leaving the
                 * CLOSING quote to start a runaway quote scan that swallowed the
                 * following tokens (e.g. the `in` of a `case`). */
                wordbuf_append(l, '$');
                lexer_ungetc(l, next);
            } else if (next != EOF && !is_name_char((unsigned char)next) &&
                       !is_special_param_char(next)) {
                /* Nothing an expansion could continue with: the `$` is literal
                 * and the next character keeps its own meaning. Appending it
                 * blindly pulled operators into the word -- `echo $;` made the
                 * word `$;`, so the `;` never terminated the command. */
                wordbuf_append(l, '$');
                lexer_ungetc(l, next);
            } else {
                wordbuf_append(l, '$');
                if (next != EOF) {
                    wordbuf_append(l, (char)next);
                }
            }
        } else if (c == '`') {
            scan_backquote(l);
        } else {
            /* Check for word-terminating characters via LUT (one table lookup
                 * EOF is handled by the loop tail; here c is always a real byte). */
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

/* lexer_read_raw() has too many return points to stamp each one, so the
 * blank-before flag is collected on the lexer and attached here. */
static token_t lexer_read(lexer_t *l)
{
    l->blank_before = 0;
    token_t t = lexer_read_raw(l);
    t.blank_before = l->blank_before;
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
