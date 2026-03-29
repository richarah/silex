#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "redirect.h"
#include "shell.h"
#include "expand.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Save fd orig_fd into a new fd (CLOEXEC), record in ctx */
static int save_fd(redirect_ctx_t *ctx, int orig_fd, arena_t *arena)
{
    int saved = fcntl(orig_fd, F_DUPFD_CLOEXEC, 3);
    if (saved < 0) {
        /* fd may not be open; that is acceptable — record -1 so restore skips */
        saved = -1;
    }

    saved_fd_t *entry = arena_alloc(arena, sizeof(saved_fd_t));
    entry->orig_fd  = orig_fd;
    entry->saved_fd = saved;
    entry->next     = ctx->saved;
    ctx->saved      = entry;
    return 0;
}

int redirect_apply(struct shell_ctx *sh, redir_t *redirs, redirect_ctx_t *ctx)
{
    ctx->saved = NULL;
    ctx->error = 0;

    for (redir_t *r = redirs; r != NULL; r = r->next) {
        int fd = r->fd;   /* target fd; caller sets sensible default */

        /* Expand the target word (except for heredocs which are literal) */
        char *target = NULL;
        if (r->op != TOK_DLESS && r->op != TOK_DLESSDASH) {
            target = expand_word(sh, r->target);
            if (!target)
                target = r->target;
        }

        /* Save the original fd before we touch it */
        save_fd(ctx, fd, sh->parse_arena.head ? sh->vars.arena : &sh->parse_arena);

        int new_fd = -1;

        switch (r->op) {
        case TOK_LESS:
            new_fd = open(target, O_RDONLY);
            if (new_fd < 0) {
                perror(target);
                ctx->error = 1;
                return -1;
            }
            break;

        case TOK_GREAT:
            new_fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (new_fd < 0) {
                perror(target);
                ctx->error = 1;
                return -1;
            }
            break;

        case TOK_DGREAT:
            new_fd = open(target, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (new_fd < 0) {
                perror(target);
                ctx->error = 1;
                return -1;
            }
            break;

        case TOK_CLOBBER:
            /* >| — open for write even if noclobber is set */
            new_fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (new_fd < 0) {
                perror(target);
                ctx->error = 1;
                return -1;
            }
            break;

        case TOK_LESSAND:
            if (strcmp(target, "-") == 0) {
                close(fd);
                continue;
            } else {
                /* Validate: target must be a non-negative decimal integer */
                if (target[0] == '\0' || target[0] == '-') {
                    fprintf(stderr, "redirect: invalid fd: %s\n", target);
                    ctx->error = 1;
                    return -1;
                }
                for (const char *tp = target; *tp; tp++) {
                    if (*tp < '0' || *tp > '9') {
                        fprintf(stderr, "redirect: invalid fd: %s\n", target);
                        ctx->error = 1;
                        return -1;
                    }
                }
                int src = atoi(target);
                if (dup2(src, fd) < 0) {
                    perror("redirect <&");
                    ctx->error = 1;
                    return -1;
                }
                continue;
            }

        case TOK_GREATAND:
            if (strcmp(target, "-") == 0) {
                close(fd);
                continue;
            } else {
                /* Validate: target must be a non-negative decimal integer */
                if (target[0] == '\0' || target[0] == '-') {
                    fprintf(stderr, "redirect: invalid fd: %s\n", target);
                    ctx->error = 1;
                    return -1;
                }
                for (const char *tp = target; *tp; tp++) {
                    if (*tp < '0' || *tp > '9') {
                        fprintf(stderr, "redirect: invalid fd: %s\n", target);
                        ctx->error = 1;
                        return -1;
                    }
                }
                int src = atoi(target);
                if (dup2(src, fd) < 0) {
                    perror("redirect >&");
                    ctx->error = 1;
                    return -1;
                }
                continue;
            }

        case TOK_LESSGREAT:
            new_fd = open(target, O_RDWR | O_CREAT, 0666);
            if (new_fd < 0) {
                perror(target);
                ctx->error = 1;
                return -1;
            }
            break;

        case TOK_DLESS:
        case TOK_DLESSDASH: {
            /* heredoc: write body to a temp file, dup2 read end */
            char tmpname[] = "/tmp/matchbox_heredoc_XXXXXX";
            int tmpfd = mkstemp(tmpname);
            if (tmpfd < 0) {
                perror("mkstemp");
                ctx->error = 1;
                return -1;
            }
            unlink(tmpname); /* unlink immediately; fd keeps it alive */

            const char *body = r->heredoc ? r->heredoc : "";
            size_t blen = strlen(body);
            size_t written = 0;
            while (written < blen) {
                ssize_t n = write(tmpfd, body + written, blen - written);
                if (n < 0) {
                    perror("heredoc write");
                    close(tmpfd);
                    ctx->error = 1;
                    return -1;
                }
                written += (size_t)n;
            }
            if (lseek(tmpfd, 0, SEEK_SET) < 0) {
                perror("heredoc lseek");
                close(tmpfd);
                ctx->error = 1;
                return -1;
            }
            new_fd = tmpfd;
            break;
        }

        default:
            /* Unknown redirect op — skip */
            continue;
        }

        /* dup2 new_fd onto the target fd */
        if (new_fd >= 0) {
            if (dup2(new_fd, fd) < 0) {
                perror("dup2");
                close(new_fd);
                ctx->error = 1;
                return -1;
            }
            close(new_fd);
        }
    }

    return 0;
}

void redirect_restore(redirect_ctx_t *ctx)
{
    for (saved_fd_t *s = ctx->saved; s != NULL; s = s->next) {
        if (s->saved_fd >= 0) {
            dup2(s->saved_fd, s->orig_fd);
            close(s->saved_fd);
        }
    }
    ctx->saved = NULL;
}
