/* hashmap.c — generic open-addressing hash map */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * hashmap.c — open-addressing hash map with Fibonacci hashing and
 *             linear probing.
 *
 * Key:   uint64_t
 * Value: void *
 *
 * Hash function: Knuth / Fibonacci multiplicative hashing.
 *   slot = (key * 11400714819323198485ULL) >> (64 - log2(cap))
 *
 * The log2 of the capacity (which must be a power of two) is computed
 * once per lookup using __builtin_ctzll(cap) which counts trailing zeros
 * (equivalent to log2 for powers of two).
 *
 * Deletion uses tombstones (used == -1) so that chains are not broken.
 * Resize at 75% load: double capacity and rehash all live entries.
 */

#include "hashmap.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Fibonacci multiplicative hash constant (2^64 / phi) */
#define FIB_HASH  UINT64_C(11400714819323198485)

#define SLOT_EMPTY     0
#define SLOT_OCCUPIED  1
#define SLOT_TOMBSTONE (-1)

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Map a key to a slot index in a table of size cap (power of two). */
static inline size_t slot_index(uint64_t key, size_t cap)
{
    /* __builtin_ctzll(cap) == log2(cap) when cap is a power of two */
    unsigned shift = (unsigned)(64u - (unsigned)__builtin_ctzll((unsigned long long)cap));
    return (size_t)((key * FIB_HASH) >> shift);
}

/*
 * Find the slot for key in the given table.
 * If found (SLOT_OCCUPIED with matching key), returns its index.
 * If not found, returns the index of the first empty or tombstone slot
 * that can be used for insertion, or cap if the table is full (shouldn't
 * happen if resizing is done correctly).
 * 'found' is set to 1 if an occupied slot with matching key was found.
 */
static size_t find_slot(hm_slot_t *slots, size_t cap,
                         uint64_t key, int *found)
{
    size_t idx      = slot_index(key, cap);
    size_t first_ts = cap; /* index of first tombstone seen */

    for (size_t i = 0; i < cap; i++) {
        size_t probe = (idx + i) & (cap - 1);
        hm_slot_t *s = &slots[probe];

        if (s->used == SLOT_OCCUPIED && s->key == key) {
            *found = 1;
            return probe;
        }
        if (s->used == SLOT_EMPTY) {
            /* Key not present; return the first tombstone if we saw one,
             * otherwise this empty slot for insertion */
            *found = 0;
            return (first_ts < cap) ? first_ts : probe;
        }
        if (s->used == SLOT_TOMBSTONE && first_ts == cap) {
            first_ts = probe;
        }
    }

    /* Table full (should not reach here with proper load-factor check) */
    *found = 0;
    return (first_ts < cap) ? first_ts : cap;
}

/* Resize the table to new_cap (must be a power of two > current cap). */
static void resize(hashmap_t *m, size_t new_cap)
{
    hm_slot_t *new_slots = calloc(new_cap, sizeof(hm_slot_t));
    if (!new_slots) return; /* out of memory; leave table as-is */

    /* Rehash all live entries */
    for (size_t i = 0; i < m->cap; i++) {
        hm_slot_t *s = &m->slots[i];
        if (s->used != SLOT_OCCUPIED) continue;

        int dummy;
        size_t idx = find_slot(new_slots, new_cap, s->key, &dummy);
        new_slots[idx].key   = s->key;
        new_slots[idx].value = s->value;
        new_slots[idx].used  = SLOT_OCCUPIED;
    }

    free(m->slots);
    m->slots = new_slots;
    m->cap   = new_cap;
    /* m->count stays the same (tombstones are not counted) */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void hm_init(hashmap_t *m, size_t initial_cap)
{
    /* Ensure initial_cap is at least 8 and a power of two */
    if (initial_cap < 8) initial_cap = 8;
    /* Round up to next power of two */
    size_t cap = 1;
    while (cap < initial_cap) cap <<= 1;

    m->slots = calloc(cap, sizeof(hm_slot_t));
    m->cap   = m->slots ? cap : 0;
    m->count = 0;
}

void hm_free(hashmap_t *m)
{
    free(m->slots);
    m->slots = NULL;
    m->cap   = 0;
    m->count = 0;
}

void *hm_get(hashmap_t *m, uint64_t key)
{
    if (!m->slots || m->cap == 0) return NULL;

    int found = 0;
    size_t idx = find_slot(m->slots, m->cap, key, &found);
    if (!found) return NULL;
    return m->slots[idx].value;
}

void hm_put(hashmap_t *m, uint64_t key, void *value)
{
    if (!m->slots) return;

    /* Resize at 75% load factor */
    if (m->count + 1 > (m->cap * 3) / 4)
        resize(m, m->cap * 2);

    if (!m->slots || m->cap == 0) return;

    int found = 0;
    size_t idx = find_slot(m->slots, m->cap, key, &found);

    if (idx >= m->cap) return; /* table full; silently drop */

    if (!found) {
        /* New entry */
        m->slots[idx].key  = key;
        m->slots[idx].used = SLOT_OCCUPIED;
        m->count++;
    }
    /* Update value (works for both new and existing entries) */
    m->slots[idx].value = value;
}

void hm_delete(hashmap_t *m, uint64_t key)
{
    if (!m->slots || m->cap == 0) return;

    int found = 0;
    size_t idx = find_slot(m->slots, m->cap, key, &found);
    if (!found) return;

    /* Mark as tombstone so probing chains are preserved */
    m->slots[idx].used  = SLOT_TOMBSTONE;
    m->slots[idx].value = NULL;
    if (m->count > 0) m->count--;
}
