/* intern.c — string intern table (FNV-1a + open addressing + arena) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "intern.h"
#include "section.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Arena: linked list of fixed blocks (stable pointers) ---------------- */

#define INTERN_BLOCK_SIZE 65536  /* bytes per arena block */

typedef struct intern_block {
    struct intern_block *next;
    size_t               used;
    char                 data[INTERN_BLOCK_SIZE];
} intern_block_t;

static intern_block_t *g_arena = NULL;

static char *arena_push(const char *s, size_t len)
{
    size_t need = len + 1; /* +1 for NUL */
    if (need > INTERN_BLOCK_SIZE) return NULL; /* string exceeds block size */

    /* Allocate new block if current one is too full */
    if (!g_arena || g_arena->used + need > INTERN_BLOCK_SIZE) {
        intern_block_t *b = malloc(sizeof(intern_block_t));
        if (!b) return NULL;
        b->next  = g_arena;
        b->used  = 0;
        g_arena  = b;
    }

    char *p = g_arena->data + g_arena->used;
    memcpy(p, s, len);
    p[len] = '\0';
    g_arena->used += need;
    return p;
}

static void arena_reset(void)
{
    while (g_arena) {
        intern_block_t *next = g_arena->next;
        free(g_arena);
        g_arena = next;
    }
}

/* ---- Hash table (open addressing, linear probing) ----------------------- */

#define INTERN_INIT_CAP 4096u  /* initial slot count (power of 2) */

typedef struct {
    const char *str;    /* NULL = empty slot; pointer into arena */
    uint64_t    hash;   /* cached FNV-1a hash */
    size_t      len;    /* string length (excluding NUL) */
} intern_slot_t;

static intern_slot_t *g_slots       = NULL;
static size_t         g_cap         = 0;
static size_t         g_count       = 0;
static size_t         g_bytes_saved = 0;

static uint64_t fnv1a64(const char *s, size_t n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* Return an existing match or an empty slot for (s, len, hash).
 * Returns NULL only if table is completely full (should not happen). */
static intern_slot_t *slot_find(const char *s, size_t len, uint64_t hash)
{
    size_t idx = (size_t)(hash & (uint64_t)(g_cap - 1));
    size_t i;
    for (i = 0; i < g_cap; i++) {
        intern_slot_t *sl = &g_slots[idx];
        if (!sl->str)
            return sl;  /* empty slot */
        if (sl->hash == hash && sl->len == len &&
            memcmp(sl->str, s, len) == 0)
            return sl;  /* existing match */
        idx = (idx + 1) & (g_cap - 1);
    }
    return NULL; /* table full */
}

static int table_init(void)
{
    g_slots = calloc(INTERN_INIT_CAP, sizeof(intern_slot_t));
    if (!g_slots) return -1;
    g_cap = INTERN_INIT_CAP;
    return 0;
}

/* Grow table to 2× current capacity and rehash all entries */
static int table_grow(void)
{
    size_t newcap = g_cap * 2;
    intern_slot_t *ns = calloc(newcap, sizeof(intern_slot_t));
    if (!ns) return -1;

    size_t i;
    for (i = 0; i < g_cap; i++) {
        intern_slot_t *old = &g_slots[i];
        if (!old->str) continue;
        size_t idx = (size_t)(old->hash & (uint64_t)(newcap - 1));
        size_t j;
        for (j = 0; j < newcap; j++) {
            if (!ns[idx].str) {
                ns[idx] = *old;
                break;
            }
            idx = (idx + 1) & (newcap - 1);
        }
    }
    free(g_slots);
    g_slots = ns;
    g_cap   = newcap;
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

HOT const char *intern_cstrn(const char *s, size_t n)
{
    if (!s) return "";

    if (!g_slots && table_init() != 0) return s; /* OOM: return original */

    uint64_t hash = fnv1a64(s, n);

    /* Check if already interned */
    intern_slot_t *sl = slot_find(s, n, hash);
    if (sl && sl->str) {
        g_bytes_saved += n + 1;
        return sl->str;
    }

    /* Grow at 75% load or if table is full */
    if (!sl || g_count * 4u >= g_cap * 3u) {
        if (table_grow() != 0) return s;
        sl = slot_find(s, n, hash);
        if (!sl) return s; /* shouldn't happen after grow */
    }

    /* Insert new string into arena and table */
    char *copy = arena_push(s, n);
    if (!copy) return s; /* OOM: return original */

    sl->str  = copy;
    sl->hash = hash;
    sl->len  = n;
    g_count++;
    return copy;
}

HOT const char *intern_cstr(const char *s)
{
    if (!s) return "";
    return intern_cstrn(s, strlen(s));
}

COLD void intern_reset(void)
{
    arena_reset();
    free(g_slots);
    g_slots       = NULL;
    g_cap         = 0;
    g_count       = 0;
    g_bytes_saved = 0;
}

size_t intern_count(void)       { return g_count; }
size_t intern_bytes_saved(void) { return g_bytes_saved; }
