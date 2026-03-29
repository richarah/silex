/* arena.h — arena allocator for matchbox shell */
#ifndef MATCHBOX_ARENA_H
#define MATCHBOX_ARENA_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>

#define ARENA_BLOCK_SIZE 65536

typedef struct arena_block {
    char              *base;
    size_t             used;
    size_t             cap;
    struct arena_block *next;
} arena_block_t;

typedef struct {
    arena_block_t *head;
} arena_t;

void  arena_init(arena_t *a);
void *arena_alloc(arena_t *a, size_t size);   /* aborts on OOM */
char *arena_strdup(arena_t *a, const char *s);
char *arena_strndup(arena_t *a, const char *s, size_t n);
void  arena_reset(arena_t *a);   /* reuse memory, do not free */
void  arena_free(arena_t *a);    /* free all blocks */

#endif /* MATCHBOX_ARENA_H */
