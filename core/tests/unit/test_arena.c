/* test_arena.c — arena allocator invariants.
 *
 * The regression this exists for: arena_reset() zeroes `used` on every block,
 * but arena_alloc() only ever consulted a->head. Every block below head was
 * empty, reachable, and never used again -- so any reset cycle needing more
 * than one block's worth pushed a brand new block, permanently, and
 * total_bytes climbed with every cycle until it hit ARENA_MAX_BYTES and
 * abort()ed.
 *
 * Measured before the fix: 262 KB after one cycle, 39 MB after two hundred --
 * 150x, and a crash at the 64 MB cap. A long ./configure does exactly this.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "util/arena.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", desc);
    if (!ok) failures++;
}

/* Capacity must be REUSED across reset cycles, not re-grown. */
static void test_reset_reuses_capacity(void)
{
    arena_t a;
    arena_init(&a);

    size_t baseline = 0;
    for (int cycle = 0; cycle < 5000; cycle++) {
        /* Each cycle spans several blocks, so the naive version is forced to
         * grow -- which is precisely the path that used to leak. */
        for (int i = 0; i < 4; i++)
            (void)arena_alloc(&a, ARENA_BLOCK_SIZE / 2 + 64);
        arena_reset(&a);
        if (cycle == 0) baseline = a.total_bytes;
    }

    check("arena: capacity plateaus across reset cycles (does not re-grow)",
          baseline > 0 && a.total_bytes == baseline);
    arena_free(&a);
}

/* An allocation must be usable and distinct from its neighbours. */
static void test_alloc_is_sane(void)
{
    arena_t a;
    arena_init(&a);

    char *p = arena_alloc(&a, 16);
    char *q = arena_alloc(&a, 16);
    memset(p, 'a', 16);
    memset(q, 'b', 16);

    check("arena: allocations do not overlap", p != q && p[0] == 'a' && q[0] == 'b');
    check("arena: pointers are aligned",
          ((uintptr_t)p % sizeof(void *)) == 0 && ((uintptr_t)q % sizeof(void *)) == 0);

    /* Reused memory after a reset must still be writable. */
    arena_reset(&a);
    char *r = arena_alloc(&a, 16);
    memset(r, 'c', 16);
    check("arena: memory is usable after reset", r[0] == 'c');

    arena_free(&a);
}

/* A block-busting allocation gets its own block and still works. */
static void test_oversized_alloc(void)
{
    arena_t a;
    arena_init(&a);

    size_t big = ARENA_BLOCK_SIZE * 2;
    char *p = arena_alloc(&a, big);
    memset(p, 'z', big);
    check("arena: allocation larger than a block succeeds",
          p != NULL && p[0] == 'z' && p[big - 1] == 'z');

    arena_free(&a);
}

static void test_strdup(void)
{
    arena_t a;
    arena_init(&a);

    char *s = arena_strdup(&a, "hello");
    check("arena: strdup copies the string", s && strcmp(s, "hello") == 0);

    arena_free(&a);
}

int main(void)
{
    printf("=== test_arena ===\n");
    test_alloc_is_sane();
    test_oversized_alloc();
    test_strdup();
    test_reset_reuses_capacity();

    printf("\nResults: %s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
