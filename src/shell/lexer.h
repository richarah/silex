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
} lexer_t;

void    lexer_init_str(lexer_t *l, const char *input, arena_t *a);
void    lexer_init_fp(lexer_t *l, FILE *fp, arena_t *a);
void    lexer_free(lexer_t *l);
token_t lexer_next(lexer_t *l);
token_t lexer_peek(lexer_t *l);
void    lexer_consume(lexer_t *l);

#endif /* SILEX_LEXER_H */
