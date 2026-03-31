#ifndef SILEX_EXEC_H
#define SILEX_EXEC_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "parser.h"
#include "shell.h"

int exec_node(shell_ctx_t *sh, node_t *node);
int exec_simple_cmd(shell_ctx_t *sh, char **words, char **assigns, redir_t *redirs);
int exec_pipeline(shell_ctx_t *sh, node_t *node);

#endif
