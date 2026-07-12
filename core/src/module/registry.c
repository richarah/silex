/* registry.c — module registry: discovery, validation, and dispatch */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "registry.h"
#include "loader.h"
#include "../../silex_module.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

/*
 * registry.c — module registry for silex.
 *
 * Modules are discovered by scanning directories listed in the
 * SILEX_MODULE_PATH environment variable (colon-separated), falling back
 * to /usr/lib/silex/modules/.
 *
 * Registry entries are hashed by "tool:flag" using FNV-1a and stored in
 * 64 buckets.  The directory mtime is checked on each lookup to detect new
 * or removed modules and invalidate the cache accordingly.
 */

#define REGISTRY_BUCKETS     64
#define REGISTRY_MAX_ENTRIES 1024   /* hard cap: stop loading beyond this many modules */
#define DEFAULT_MODULE_DIR   "/usr/lib/silex/modules"

typedef struct reg_entry {
    char               *tool;
    char               *flag;
    char               *so_path;
    silex_module_t  *mod;
    struct reg_entry   *next;
} reg_entry_t;

static reg_entry_t *buckets[REGISTRY_BUCKETS];
static time_t       dir_mtime;     /* mtime of the last-checked module dir */
static int          registry_count; /* total entries across all buckets */

/* FNV-1a hash over a NUL-terminated string */
static uint32_t fnv1a(const char *s)
{
    uint32_t hash = 2166136261u;
    for (; *s; s++) {
        hash ^= (unsigned char)*s;
        hash *= 16777619u;
    }
    return hash;
}

/* Compute bucket index for (tool, flag) */
static size_t bucket_index(const char *tool, const char *flag)
{
    /* Hash "tool:flag" */
    char key[512];
    snprintf(key, sizeof(key), "%s:%s", tool, flag);
    return fnv1a(key) & (REGISTRY_BUCKETS - 1);
}

/* Free a single entry and its heap strings (but NOT the module itself) */
static void entry_free(reg_entry_t *e)
{
    if (!e) return;
    free(e->tool);
    free(e->flag);
    free(e->so_path);
    free(e);
}

/* Free all registry entries */
static void registry_clear(void)
{
    for (int i = 0; i < REGISTRY_BUCKETS; i++) {
        reg_entry_t *e = buckets[i];
        while (e) {
            reg_entry_t *next = e->next;
            entry_free(e);
            e = next;
        }
        buckets[i] = NULL;
    }
    registry_count = 0;
}

/*
 * Determine the first module directory to check.
 * If SILEX_MODULE_PATH is set, use its first component.
 * Otherwise use the default.
 */
static void first_module_dir(char out[PATH_MAX])
{
    const char *env = getenv("SILEX_MODULE_PATH");
    if (env && *env) {
        /* Take first colon-separated component */
        const char *colon = strchr(env, ':');
        size_t len = colon ? (size_t)(colon - env) : strlen(env);
        if (len >= PATH_MAX) len = PATH_MAX - 1;
        memcpy(out, env, len);
        out[len] = '\0';
    } else {
        snprintf(out, PATH_MAX, "%s", DEFAULT_MODULE_DIR);
    }
}

void registry_check_invalidate(void)
{
    char dir[PATH_MAX];
    first_module_dir(dir);

    struct stat st;
    if (stat(dir, &st) != 0) {
        /* Directory does not exist — clear any cached entries */
        if (dir_mtime != 0) {
            registry_clear();
            dir_mtime = 0;
        }
        return;
    }

    if (st.st_mtime != dir_mtime) {
        registry_clear();
        dir_mtime = st.st_mtime;
    }
}

silex_module_t *registry_find(const char *tool, const char *flag)
{
    size_t idx = bucket_index(tool, flag);
    for (reg_entry_t *e = buckets[idx]; e; e = e->next) {
        if (strcmp(e->tool, tool) == 0 && strcmp(e->flag, flag) == 0)
            return e->mod;
    }
    return NULL;
}

void registry_register(const char *tool, const char *flag,
                        const char *so_path, silex_module_t *mod)
{
    size_t idx = bucket_index(tool, flag);

    /* Avoid duplicate registrations */
    for (reg_entry_t *e = buckets[idx]; e; e = e->next) {
        if (strcmp(e->tool, tool) == 0 && strcmp(e->flag, flag) == 0)
            return;
    }

    /* Hard cap: refuse to grow beyond REGISTRY_MAX_ENTRIES */
    if (registry_count >= REGISTRY_MAX_ENTRIES) return;

    reg_entry_t *e = calloc(1, sizeof(*e));
    if (!e) return;

    e->tool    = strdup(tool);
    e->flag    = strdup(flag);
    e->so_path = strdup(so_path);
    e->mod     = mod;

    if (!e->tool || !e->flag || !e->so_path) {
        entry_free(e);
        return;
    }

    e->next       = buckets[idx];
    buckets[idx]  = e;
    registry_count++;
}

/*
 * Check whether a .so file passes the minimal security checks needed for
 * scanning (skip symlinks and world-writable files owned by others).
 * Full checks are done by module_load(); this is just a quick pre-filter.
 */
static int so_passes_scan_checks(const char *path)
{
    uid_t my_uid = getuid();
    struct stat lst;

    if (lstat(path, &lst) != 0)
        return 0;
    if (S_ISLNK(lst.st_mode))
        return 0;
    if (lst.st_mode & S_IWOTH)
        return 0;
    if (lst.st_uid != 0 && lst.st_uid != my_uid)
        return 0;
    return 1;
}

/*
 * Scan one directory for a module supporting (tool, flag).
 * Returns the first matching module pointer, or NULL.
 */
static silex_module_t *scan_dir(const char *dir,
                                    const char *tool, const char *flag)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;

    struct dirent *ent;
    silex_module_t *found = NULL;

    while (!found && (ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t namelen = strlen(name);

        /* Only consider files ending in ".so" */
        if (namelen < 4 || strcmp(name + namelen - 3, ".so") != 0)
            continue;

        char so_path[PATH_MAX];
        int r = snprintf(so_path, sizeof(so_path), "%s/%s", dir, name);
        if (r < 0 || (size_t)r >= sizeof(so_path))
            continue;

        if (!so_passes_scan_checks(so_path))
            continue;

        silex_module_t *mod = module_load(so_path);
        if (!mod)
            continue;

        /* Check if this module handles the requested tool */
        if (!mod->tool_name || strcmp(mod->tool_name, tool) != 0)
            continue;

        /* Check if the extra_flags list contains the requested flag */
        if (!mod->extra_flags)
            continue;

        int flag_match = 0;
        for (const char **f = mod->extra_flags; *f; f++) {
            if (strcmp(*f, flag) == 0) {
                flag_match = 1;
                break;
            }
        }

        if (!flag_match)
            continue;

        /* Register all flags this module advertises for this tool */
        for (const char **f = mod->extra_flags; *f; f++)
            registry_register(tool, *f, so_path, mod);

        found = mod;
    }

    closedir(d);
    return found;
}

silex_module_t *registry_lookup(const char *tool, const char *flag)
{
    /* Invalidate stale cache first */
    registry_check_invalidate();

    /* Check in-memory registry first */
    silex_module_t *cached = registry_find(tool, flag);
    if (cached)
        return cached;

    /* Scan all module directories in SILEX_MODULE_PATH */
    const char *env = getenv("SILEX_MODULE_PATH");
    if (env && *env) {
        /* Make a mutable copy to tokenise */
        char *env_copy = strdup(env);
        if (!env_copy) return NULL;

        char *saveptr = NULL;
        char *token = strtok_r(env_copy, ":", &saveptr);
        while (token) {
            silex_module_t *mod = scan_dir(token, tool, flag);
            if (mod) {
                free(env_copy);
                return mod;
            }
            token = strtok_r(NULL, ":", &saveptr);
        }
        free(env_copy);
    } else {
        /* Fall back to default module directory */
        silex_module_t *mod = scan_dir(DEFAULT_MODULE_DIR, tool, flag);
        if (mod)
            return mod;
    }

    return NULL;
}
