/* fscache.c — filesystem path stat cache with TTL invalidation */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * fscache.c — per-process filesystem stat cache.
 *
 * Caches the result of stat()/lstat() keyed by FNV-1a(path).
 * TTL is read from MATCHBOX_FSCACHE_TTL (seconds); 0 disables the cache
 * (all calls pass through directly to the kernel).
 *
 * The cache is not thread-safe; it is designed for single-threaded use
 * inside a container build runtime process.
 *
 * Key encoding:
 *   stat()  entries: FNV-1a(path)
 *   lstat() entries: FNV-1a(path) | FSCACHE_LSTAT_FLAG
 *
 * The path string is stored in each entry for collision detection.
 * A full struct stat is cached (no partial field storage).
 *
 * Cache invalidation:
 *   fscache_invalidate(path) removes the entry for path AND its parent
 *   directory, because operations on a path (e.g. mkdir, unlink) change
 *   the parent's mtime/nlink.  No stat() call is needed for invalidation;
 *   we hash the path string directly.
 */

#include "fscache.h"
#include "hashmap.h"
#include "../util/intern.h"
#include "../util/section.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Default initial capacity for the hashmap */
#define FSCACHE_INITIAL_CAP  256

/* Hard cap on the number of cached entries.  A container build script will
 * never stat 100 000 distinct paths; this prevents unbounded memory growth. */
#define FSCACHE_MAX_ENTRIES  100000

/* Default TTL in seconds when MATCHBOX_FSCACHE_TTL is not set */
#define FSCACHE_DEFAULT_TTL 5

/* Global cache instance */
fscache_t g_fscache;

/* ------------------------------------------------------------------ */
/* FNV-1a 64-bit path hash                                             */
/* ------------------------------------------------------------------ */

static uint64_t fnv1a_path(const char *path)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        h ^= (uint64_t)*p;
        h *= UINT64_C(1099511628211);
    }
    /* Reserve the high bit for FSCACHE_LSTAT_FLAG; fold it down if set */
    return h & ~FSCACHE_LSTAT_FLAG;
}

/* ------------------------------------------------------------------ */
/* Init / free                                                          */
/* ------------------------------------------------------------------ */

COLD void fscache_init(void)
{
    hm_init(&g_fscache.map, FSCACHE_INITIAL_CAP);

    const char *env = getenv("MATCHBOX_FSCACHE_TTL");
    if (env) {
        long v = strtol(env, NULL, 10);
        g_fscache.ttl = (v > 0) ? (int)v : 0;
    } else {
        g_fscache.ttl = FSCACHE_DEFAULT_TTL;
    }
}

COLD void fscache_free(void)
{
    if (g_fscache.map.slots) {
        for (size_t i = 0; i < g_fscache.map.cap; i++) {
            hm_slot_t *s = &g_fscache.map.slots[i];
            if (s->used == 1 && s->value) {
                fscache_entry_t *e = (fscache_entry_t *)s->value;
                /* e->path is interned; owned by intern table, not freed here */
                free(e);
                s->value = NULL;
            }
        }
    }
    hm_free(&g_fscache.map);
    g_fscache.ttl = 0;
}

/* ------------------------------------------------------------------ */
/* Internal: store a stat result in the cache                          */
/* ------------------------------------------------------------------ */

/*
 * Store *st in the cache at the given key, associated with path.
 * If a collision is detected (different path, same hash), the old entry
 * is replaced.
 */
static void cache_store(const char *path, uint64_t key, const struct stat *st)
{
    if (g_fscache.ttl <= 0)
        return;

    fscache_entry_t *entry = (fscache_entry_t *)hm_get(&g_fscache.map, key);

    if (entry) {
        if (strcmp(entry->path, path) != 0) {
            /* Hash collision: different path maps to same key.
             * Replace the old entry (old path is interned; not freed). */
            entry->path = intern_cstr(path);
        }
    } else {
        /* Enforce entry count cap before allocating new entry */
        if (g_fscache.map.count >= FSCACHE_MAX_ENTRIES)
            return;  /* cap reached: skip caching without eviction */

        entry = malloc(sizeof(*entry));
        if (!entry)
            return; /* OOM: skip caching */
        entry->path = intern_cstr(path);
        hm_put(&g_fscache.map, key, entry);
    }

    entry->st        = *st;
    entry->cached_at = time(NULL);
}

/* ------------------------------------------------------------------ */
/* Internal: look up cache by path+key                                 */
/* ------------------------------------------------------------------ */

/*
 * Returns a valid non-expired entry for path, or NULL on miss/expiry.
 * Does NOT call stat(); pure cache lookup only.
 */
static fscache_entry_t *cache_lookup(const char *path, uint64_t key)
{
    fscache_entry_t *entry = (fscache_entry_t *)hm_get(&g_fscache.map, key);
    if (!entry)
        return NULL;

    /* Collision check */
    if (strcmp(entry->path, path) != 0)
        return NULL;

    /* TTL check */
    if (time(NULL) - entry->cached_at >= (time_t)g_fscache.ttl)
        return NULL;

    return entry;
}

/* ------------------------------------------------------------------ */
/* fscache_stat                                                         */
/* ------------------------------------------------------------------ */

HOT int fscache_stat(const char *path, struct stat *out)
{
    /* Fast path: TTL disabled */
    if (g_fscache.ttl <= 0)
        return stat(path, out);

    uint64_t key = fnv1a_path(path);

    /* Cache hit: return without calling stat() */
    fscache_entry_t *entry = cache_lookup(path, key);
    if (entry) {
        *out = entry->st;
        return 0;
    }

    /* Cache miss or expired: call stat() and populate cache */
    struct stat fresh;
    if (stat(path, &fresh) != 0)
        return -1;

    cache_store(path, key, &fresh);
    *out = fresh;
    return 0;
}

/* ------------------------------------------------------------------ */
/* fscache_lstat                                                        */
/* ------------------------------------------------------------------ */

WARM int fscache_lstat(const char *path, struct stat *out)
{
    /* Fast path: TTL disabled */
    if (g_fscache.ttl <= 0)
        return lstat(path, out);

    /* lstat uses a separate key namespace to avoid aliasing with stat() */
    uint64_t key = fnv1a_path(path) | FSCACHE_LSTAT_FLAG;

    /* Cache hit: return without calling lstat() */
    fscache_entry_t *entry = cache_lookup(path, key);
    if (entry) {
        *out = entry->st;
        return 0;
    }

    /* Cache miss or expired: call lstat() and populate cache */
    struct stat fresh;
    if (lstat(path, &fresh) != 0)
        return -1;

    cache_store(path, key, &fresh);
    *out = fresh;
    return 0;
}

/* ------------------------------------------------------------------ */
/* fscache_invalidate                                                   */
/* ------------------------------------------------------------------ */

/*
 * Invalidate cache entries for path and its parent directory.
 * Uses path hashing only — no stat() call required for invalidation.
 */

static void invalidate_one(const char *p)
{
    uint64_t key  = fnv1a_path(p);
    uint64_t lkey = key | FSCACHE_LSTAT_FLAG;

    fscache_entry_t *e = (fscache_entry_t *)hm_get(&g_fscache.map, key);
    if (e && strcmp(e->path, p) == 0) {
        /* e->path is interned; owned by intern table, not freed here */
        free(e);
        hm_delete(&g_fscache.map, key);
    }

    fscache_entry_t *le = (fscache_entry_t *)hm_get(&g_fscache.map, lkey);
    if (le && strcmp(le->path, p) == 0) {
        /* le->path is interned; owned by intern table, not freed here */
        free(le);
        hm_delete(&g_fscache.map, lkey);
    }
}

/*
 * Invalidate ALL cached entries.  Call after fork+exec of an external command
 * because the external command may have changed arbitrary filesystem state.
 */
void fscache_invalidate_all(void)
{
    if (!g_fscache.map.slots)
        return;
    /* Free all entries, then reinitialise the hashmap to a clean state */
    for (size_t i = 0; i < g_fscache.map.cap; i++) {
        hm_slot_t *s = &g_fscache.map.slots[i];
        if (s->used == 1 && s->value) {
            free(s->value);
        }
        s->value = NULL;
        s->used  = 0;  /* SLOT_EMPTY */
    }
    g_fscache.map.count = 0;
}

/*
 * Insert a freshly-observed stat result (called after successful builtin ops).
 * Sets written_by_matchbox = 1 to allow XC-01/XC-02 optimisations.
 */
void fscache_insert(const char *path, const struct stat *st)
{
    if (g_fscache.ttl <= 0 || !path || !st)
        return;
    uint64_t key = fnv1a_path(path);
    fscache_entry_t *entry = (fscache_entry_t *)hm_get(&g_fscache.map, key);
    if (!entry) {
        if (g_fscache.map.count >= FSCACHE_MAX_ENTRIES)
            return;
        entry = malloc(sizeof(*entry));
        if (!entry)
            return;
        entry->path = intern_cstr(path);
        hm_put(&g_fscache.map, key, entry);
    } else if (strcmp(entry->path, path) != 0) {
        entry->path = intern_cstr(path);
    }
    entry->st                 = *st;
    entry->cached_at          = time(NULL);
    entry->written_by_matchbox = 1;
}

int fscache_written_by_matchbox(const char *path)
{
    if (!path || g_fscache.ttl <= 0)
        return 0;
    uint64_t key = fnv1a_path(path);
    fscache_entry_t *e = (fscache_entry_t *)hm_get(&g_fscache.map, key);
    if (!e || strcmp(e->path, path) != 0)
        return 0;
    if (time(NULL) - e->cached_at >= (time_t)g_fscache.ttl)
        return 0;
    return e->written_by_matchbox ? 1 : 0;
}

WARM void fscache_invalidate(const char *path)
{
    if (!path) return;

    invalidate_one(path);

    /* Invalidate the parent directory */
    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        /* e.g. "/usr/local/bin" -> parent "/usr/local" */
        size_t len = (size_t)(slash - path);
        if (len < PATH_MAX) {
            char parent[PATH_MAX];
            memcpy(parent, path, len);
            parent[len] = '\0';
            invalidate_one(parent);
        }
    } else if (slash == path) {
        /* path is directly under root: parent is "/" */
        invalidate_one("/");
    } else {
        /* No slash: path is relative, parent is "." */
        invalidate_one(".");
    }
}
