#ifndef MATCHBOX_FSCACHE_H
#define MATCHBOX_FSCACHE_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <sys/stat.h>
#include <time.h>
#include "hashmap.h"

/*
 * fscache_entry_t — one cached path lookup result.
 *
 * Keyed by FNV-1a(path) in the hashmap.  The path string is stored so
 * that collisions (two different paths mapping to the same 64-bit hash)
 * can be detected and handled correctly.
 *
 * stat() and lstat() results for the same path are stored under different
 * keys: lstat entries use key | FSCACHE_LSTAT_FLAG to prevent aliasing
 * when a path is a symlink.
 */
typedef struct {
    const char *path;      /* interned path (for collision detection; owned by intern table) */
    struct stat st;        /* full cached stat result                  */
    time_t      cached_at; /* unix time of population                  */
} fscache_entry_t;

/* High-bit flag distinguishing lstat entries from stat entries */
#define FSCACHE_LSTAT_FLAG UINT64_C(0x8000000000000000)

typedef struct {
    hashmap_t  map;    /* key: FNV-1a(path) [stat] or FNV-1a(path)|LSTAT_FLAG [lstat] */
    int        ttl;    /* seconds; 0 = disabled (pass-through to kernel)               */
} fscache_t;

extern fscache_t g_fscache;  /* global per-process cache */

void fscache_init(void);
void fscache_free(void);

/* Look up or populate cache entry. Returns cached stat or calls stat(). */
int fscache_stat(const char *path, struct stat *out);
int fscache_lstat(const char *path, struct stat *out);

/* Invalidate a path and its parent */
void fscache_invalidate(const char *path);

#endif /* MATCHBOX_FSCACHE_H */
