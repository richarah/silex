#ifndef SILEX_PARSER_H
#define SILEX_PARSER_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lexer.h"
#include "../util/arena.h"

typedef enum {
    N_CMD,
    N_PIPE,
    N_AND,
    N_OR,
    N_NOT,
    N_SEQ,
    N_ASYNC,
    N_SUBSHELL,
    N_BRACE,
    N_IF,
    N_WHILE,
    N_UNTIL,
    N_FOR,
    N_CASE,
    N_FUNC,
    N_REDIR,
} node_type_t;

typedef struct redir {
    int           fd;
    tok_type_t    op;
    char         *target;
    char         *heredoc;
    int           heredoc_no_expand; /* 1 if delimiter was quoted — no variable expansion */
    struct redir *next;
} redir_t;

typedef struct case_item {
    char            **patterns;
    struct node     *body;
    struct case_item *next;
} case_item_t;

typedef struct node {
    node_type_t  type;
    arena_t     *arena;
    union {
        struct { char **words; char **assigns; redir_t *redirs; } cmd;
        struct { struct node *left; struct node *right; } binary;
        struct { struct node *body; redir_t *redirs; } redir_node;
        struct {
            struct node *cond;
            struct node *then_b;
            struct node *elif_chain;
            struct node *else_b;
        } if_node;
        struct { struct node *cond; struct node *body; } loop;
        struct { char *var; char **words; struct node *body; } for_node;
        struct { char *word; case_item_t *items; } case_node;
        struct { char *name; struct node *body; } func;
    } u;
} node_t;

typedef struct {
    lexer_t *lexer;
    arena_t *arena;
    int      error;   /* set on parse error */
} parser_t;

void    parser_init(parser_t *p, lexer_t *l, arena_t *a);
node_t *parser_parse(parser_t *p);            /* parse one complete command */
node_t *parser_parse_list(parser_t *p);       /* parse until EOF */
void    parser_error(parser_t *p, const char *msg);

#endif /* SILEX_PARSER_H */
