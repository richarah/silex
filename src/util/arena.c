/* arena.c — arena allocator for matchbox shell */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "arena.h"
#include "section.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

COLD void arena_init(arena_t *a)
{
    a->head        = NULL;
    a->total_bytes = 0;
}

HOT void *arena_alloc(arena_t *a, size_t size)
{
    /* Round up to sizeof(void*) alignment */
    size_t align = sizeof(void *);
    size = (size + align - 1) & ~(align - 1);

    /* Allocate a new block if needed (slow path — uncommon) */
    if (unlikely(a->head == NULL || (a->head->cap - a->head->used) < size)) {
        size_t block_data_size = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;

        if (a->total_bytes + block_data_size > ARENA_MAX_BYTES) {
            fprintf(stderr, "matchbox: arena: exceeded maximum size (%u MB)\n",
                    (unsigned)(ARENA_MAX_BYTES / (1024u * 1024u)));
            abort();
        }

        arena_block_t *blk = malloc(sizeof(arena_block_t));
        if (!blk)
            abort();

        blk->base = malloc(block_data_size);
        if (!blk->base) {
            free(blk);
            abort();
        }

        blk->cap          = block_data_size;
        blk->used         = 0;
        blk->next         = a->head;
        a->head           = blk;
        a->total_bytes   += block_data_size;
    }

    void *ptr = (void *)(a->head->base + a->head->used);
    a->head->used += size;
    return ptr;
}

char *arena_strdup(arena_t *a, const char *s)
{
    size_t len = strlen(s);
    char  *dst = arena_alloc(a, len + 1);
    memcpy(dst, s, len + 1);
    return dst;
}

char *arena_strndup(arena_t *a, const char *s, size_t n)
{
    char *dst = arena_alloc(a, n + 1);
    memcpy(dst, s, n);
    dst[n] = '\0';
    return dst;
}

void arena_reset(arena_t *a)
{
    /* Reuse existing block capacity — do not free blocks, just reset offsets.
     * total_bytes is preserved: it tracks allocated capacity, not current usage. */
    for (arena_block_t *blk = a->head; blk != NULL; blk = blk->next)
        blk->used = 0;
}

void arena_free(arena_t *a)
{
    arena_block_t *blk = a->head;
    while (blk != NULL) {
        arena_block_t *next = blk->next;
        free(blk->base);
        free(blk);
        blk = next;
    }
    a->head = NULL;
}
