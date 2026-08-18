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

/* Text spliced into the CHARACTER stream ahead of the real input, so an alias
 * value lexes exactly as if it had been typed in place.
 *
 * The alternative — lexing the value with a separate lexer and handing the
 * parser the resulting tokens — cannot express the two things POSIX allows an
 * alias value to do: leave a quote open for the text that follows it
 * (`alias e_='echo "'`), and start a here-doc whose body is part of the value.
 * A token that begins in the value and ends in the real input is one token
 * here, and a here-doc started inside the value is collected by the same lexer
 * that is collecting every other here-doc, in the same order.
 *
 * `name` is the alias this text came from. It stays on the stack until the
 * text runs out, and an alias on the stack is not substituted again — that is
 * the whole of the recursion guard, and it is why `alias ls='ls -la'` expands
 * once rather than forever while `alias a=b; alias b=a` terminates. */
typedef struct alias_push {
    char              *text;       /* malloc'd copy; the alias table may change */
    size_t             pos;
    char              *name;       /* malloc'd; NULL for a re-pushed character */
    int                ends_blank; /* value ended in <blank>: see alias_blank */
    /* The text has run out but the entry is still here. An exhausted value is
     * kept for one more character read so that the alias stays guarded until
     * the token whose last character came from it has been handed to the
     * parser. Without that grace `alias e_=e_` re-expands: the guard would be
     * gone by the time the parser looked at the e_ the value produced. */
    int                spent;
    struct alias_push *next;
} alias_push_t;

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
    /* Alias text still being read, innermost first. */
    alias_push_t      *pushed;
    int                pushed_depth;
    /* Raised when the text of an alias whose value ended in a <blank> runs
     * out. POSIX 2.3.1: the word FOLLOWING such a value is itself checked for
     * alias substitution, which is what makes `alias e_='echo '` chain into a
     * second alias. The parser reads it with lexer_take_alias_blank() right
     * after the peek that crossed the boundary. */
    int                alias_blank;
} lexer_t;

void    lexer_init_str(lexer_t *l, const char *input, arena_t *a);
void    lexer_init_fp(lexer_t *l, FILE *fp, arena_t *a);
/* Echo input to stderr as it is read while *flag is non-zero (set -v). */
void    lexer_set_verbose(lexer_t *l, const int *flag);
void    lexer_free(lexer_t *l);
token_t lexer_next(lexer_t *l);
token_t lexer_peek(lexer_t *l);
void    lexer_consume(lexer_t *l);

/* Splice an alias value into the character stream. `name` guards against
 * re-substituting the same alias while its text is being read. Returns 0 if
 * the stack is full (a pathological alias chain), in which case nothing was
 * pushed and the caller must treat the word as an ordinary command. */
int     lexer_push_alias(lexer_t *l, const char *value, const char *name);
/* 1 while `name`'s value is still being read — do not substitute it again. */
int     lexer_alias_active(const lexer_t *l, const char *name);
/* Read and clear the blank-continuation flag described above. */
int     lexer_take_alias_blank(lexer_t *l);

#endif /* SILEX_LEXER_H */
