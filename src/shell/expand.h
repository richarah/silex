#ifndef MATCHBOX_EXPAND_H
#define MATCHBOX_EXPAND_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../util/arena.h"
#include "vars.h"
#include <stddef.h>

/* Result of expanding a word: array of strings (after field splitting) */
typedef struct {
    char  **words;   /* NULL-terminated, arena-allocated */
    int     count;
} expand_result_t;

struct shell_ctx; /* forward declaration */

/* Expand a single word (no field splitting yet) */
char *expand_word(struct shell_ctx *sh, const char *word);

/* Expand a word and perform field splitting + globbing */
expand_result_t expand_word_full(struct shell_ctx *sh, const char *word);

/* Expand an array of words (for command arguments) */
char **expand_words(struct shell_ctx *sh, char **words);

/* Expand arithmetic expression -- returns long */
long expand_arith(struct shell_ctx *sh, const char *expr);

#endif
