#ifndef SILEX_LEXER_H
#define SILEX_LEXER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/arena.h"
#include <stdio.h>

typedef enum {
    TOK_WORD,
    TOK_ASSIGN,     /* NAME=VALUE */
    TOK_NEWLINE,
    TOK_SEMI,       /* ; */
    TOK_DSEMI,      /* ;; */
    TOK_AMP,        /* & */
    TOK_PIPE,       /* | */
    TOK_AND_AND,    /* && */
    TOK_OR_OR,      /* || */
    TOK_LESS,       /* < */
    TOK_GREAT,      /* > */
    TOK_DLESS,      /* << */
    TOK_DGREAT,     /* >> */
    TOK_LESSAND,    /* <& */
    TOK_GREATAND,   /* >& */
    TOK_LESSGREAT,  /* <> */
    TOK_DLESSDASH,  /* <<- */
    TOK_CLOBBER,    /* >| */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_BANG,       /* ! */
    TOK_IF, TOK_THEN, TOK_ELSE, TOK_ELIF, TOK_FI,
    TOK_WHILE, TOK_UNTIL, TOK_DO, TOK_DONE,
    TOK_FOR, TOK_IN, TOK_CASE, TOK_ESAC,
    TOK_FUNCTION,
    TOK_EOF,
} tok_type_t;

typedef struct {
    tok_type_t  type;
    char       *text;    /* arena-allocated; NULL for non-word tokens */
    int         lineno;
    /* 1 if blanks (space/tab) were skipped before this token. The IO_NUMBER
     * rule needs it: POSIX only reads a digit word as a file descriptor when
     * the redirect operator follows it with NO intervening blank, so
     * `seq 5 6 > f` redirects stdout and passes 6 to seq, while `seq 5 6> f`
     * redirects fd 6. */
    int         blank_before;
} token_t;

/* Heredoc pending entry */
typedef struct heredoc_pending {
    char                   *delim;
    int                     strip_tabs;  /* 1 for <<- */
    int                     no_expand;   /* 1 if delimiter was quoted — no variable expansion */
    char                  **body_out;    /* where to store the body text */
    struct heredoc_pending *next;
} heredoc_pending_t;

typedef struct {
    const char        *input;    /* NULL if reading from FILE */
    size_t             pos;      /* position in input string */
    FILE              *fp;       /* NULL if reading from string */
    int                lineno;
    arena_t           *arena;
    token_t            peek;     /* one-token lookahead */
    int                has_peek;
    heredoc_pending_t *heredocs; /* pending heredocs to read */
    /* buffer for building word tokens */
    char              *wordbuf;
    size_t             wordbuf_len;
    size_t             wordbuf_cap;
    /* one-char pushback */
    int                pushback;
    int                has_pushback;
    /* set by lexer_read()'s blank-skipping loop, harvested into the token */
    int                blank_before;
    /* set -v (verbose): points at the shell's live option flag, so a `set -v`
     * partway through a script takes effect from the next character read.
     * NULL means "no shell attached" -- the lexer echoes nothing. */
    const int         *verbose;
    char               vbuf[256];   /* echo buffer, flushed per line */
    size_t             vlen;
    /* Set (to a static string naming the construct) when a quote or
     * substitution runs into end of input without its closer. The scanners
     * cannot return a token type -- an unterminated `'` still yields a WORD --
     * so they record it here and the parser turns it into a syntax error
     * BEFORE the half-read command is handed to the executor. Without this
     * `echo 'abc` printed abc and exited 0, silently swallowing the rest of
     * the script into the runaway quote. First error wins. */
    const char        *error;
} lexer_t;

void    lexer_init_str(lexer_t *l, const char *input, arena_t *a);
void    lexer_init_fp(lexer_t *l, FILE *fp, arena_t *a);
/* Echo input to stderr as it is read while *flag is non-zero (set -v). */
void    lexer_set_verbose(lexer_t *l, const int *flag);
void    lexer_free(lexer_t *l);
token_t lexer_next(lexer_t *l);
token_t lexer_peek(lexer_t *l);
void    lexer_consume(lexer_t *l);

#endif /* SILEX_LEXER_H */
