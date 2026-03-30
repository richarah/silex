#ifndef MATCHBOX_REDIRECT_H
#define MATCHBOX_REDIRECT_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "parser.h"

/* Saved file descriptor for restoration */
typedef struct saved_fd {
    int            orig_fd;
    int            saved_fd;
    struct saved_fd *next;
} saved_fd_t;

typedef struct {
    saved_fd_t *saved;
    int         error;
} redirect_ctx_t;

struct shell_ctx;

/* Apply all redirections in the list; save originals in ctx */
int redirect_apply(struct shell_ctx *sh, redir_t *redirs, redirect_ctx_t *ctx);

/* Restore saved file descriptors */
void redirect_restore(redirect_ctx_t *ctx);

/* Commit redirections permanently (close saved fds, do not restore) */
void redirect_commit(redirect_ctx_t *ctx);

#endif
