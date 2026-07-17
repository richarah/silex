/* arena.c — arena allocator for silex shell */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "arena.h"
#include "section.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

COLD void arena_init(arena_t *a, const char *name)
{
    a->head        = NULL;
    a->total_bytes = 0;
    a->name        = name ? name : "anon";
}

HOT void *arena_alloc(arena_t *a, size_t size)
{
    /* Round up to sizeof(void*) alignment.
     * The addition can wrap for a size near SIZE_MAX, which would round down to
     * 0 and hand back a zero-length allocation the caller then writes through. */
    size_t align = sizeof(void *);
    if (unlikely(size > SIZE_MAX - (align - 1))) {
        fprintf(stderr, "silex: arena: %s: allocation too large\n", a->name);
        abort();
    }
    size = (size + align - 1) & ~(align - 1);
    if (unlikely(size == 0))
        size = align;

    /* Fast path: the current block has room. */
    if (likely(a->head != NULL && (a->head->cap - a->head->used) >= size)) {
        void *ptr = (void *)(a->head->base + a->head->used);
        a->head->used += size;
        return ptr;
    }

    /* Slow path.
     *
     * Before growing, look for capacity we already own. arena_reset() zeroes
     * `used` on EVERY block, but this function only ever consulted a->head --
     * so every block below head was empty, reachable, and never used again.
     *
     * The effect was that any reset cycle needing more than one block's worth
     * pushed a brand new block, permanently, and total_bytes climbed with every
     * cycle until it hit ARENA_MAX_BYTES and abort()ed. Measured: 262 KB after
     * one cycle, 39 MB after two hundred -- a 150x growth, and a crash at the
     * 64 MB cap. A long ./configure does exactly this workload.
     *
     * The scan is O(blocks), but only on the slow path, and the list is short
     * because it now stops growing.
     */
    for (arena_block_t *b = a->head ? a->head->next : NULL; b != NULL; b = b->next) {
        if ((b->cap - b->used) >= size) {
            void *ptr = (void *)(b->base + b->used);
            b->used += size;
            return ptr;
        }
    }

    /* Genuinely out of capacity: grow. */
    {
        size_t block_data_size = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;

        if (a->total_bytes > ARENA_MAX_BYTES - block_data_size) {
            fprintf(stderr, "silex: arena: %s: exceeded maximum size (%u MB)\n",
                    a->name, (unsigned)(ARENA_MAX_BYTES / (1024u * 1024u)));
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

        void *ptr = (void *)(a->head->base + a->head->used);
        a->head->used += size;
        return ptr;
    }
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
#ifdef ARENA_POISON
    if (getenv("SILEX_ARENA_TRACE"))
        fprintf(stderr, "[arena_reset: %s]\n", a->name);
#endif
    for (arena_block_t *blk = a->head; blk != NULL; blk = blk->next) {
#ifdef ARENA_POISON
        /* A pointer that outlives a reset is a use-after-free that neither
         * valgrind nor ASan can see: the block is still legitimately malloc'd,
         * so the stale read hits mapped memory and usually returns the old
         * bytes intact. That makes such bugs pass every test until an unrelated
         * allocation happens to land on the same offset.
         *
         * Poisoning on reset turns "usually works" into "always fails", which
         * is the only way to test lifetime assumptions about this arena. */
        memset(blk->base, 0xDD, blk->used);
#endif
        blk->used = 0;
    }

    /* Consolidate a fragmented block list into one block of the same capacity.
     *
     * arena_alloc() can only reuse a block that has `size` free in ONE piece. An
     * allocation that grows a little every cycle therefore stops being able to
     * reuse anything as soon as it exceeds ARENA_BLOCK_SIZE: each request is
     * slightly larger than any block owned, so every cycle mallocs another
     * block, and total_bytes climbs to the cap even though almost none of it is
     * live. Measured on `set -- "$@" "x"` in a loop -- the standard POSIX way to
     * build a list -- it aborted at 8192 items, exactly where the pointer array
     * first exceeds one 64 KB block, while the live set was well under 1 MB.
     *
     * Everything in the arena is dead here by definition, so swapping the list
     * for a single equally-large block is safe, and it makes that capacity
     * contiguous again.
     *
     * This is self-limiting rather than a per-reset cost: it only runs when the
     * list has more than one block, and it leaves exactly one behind, so the
     * common single-block loop never pays for it, and a grown arena pays once
     * and then stays consolidated. If the malloc fails, the plain reset above
     * has already left the arena valid -- keep the existing blocks.
     */
    if (a->head && a->head->next) {
        size_t total = a->total_bytes;
        char  *mem   = malloc(total);
        if (mem) {
            arena_block_t *blk = a->head;
            while (blk) {
                arena_block_t *next = blk->next;
                free(blk->base);
                free(blk);
                blk = next;
            }
            arena_block_t *nb = malloc(sizeof(arena_block_t));
            if (!nb) {
                /* Cannot rebuild the list: nothing is reachable any more, so
                 * this must not return with a dangling head. */
                free(mem);
                a->head        = NULL;
                a->total_bytes = 0;
                return;
            }
            nb->base       = mem;
            nb->cap        = total;
            nb->used       = 0;
            nb->next       = NULL;
            a->head        = nb;
            a->total_bytes = total;
        }
    }
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
