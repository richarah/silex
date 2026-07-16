/* vars.c — shell variable store */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "vars.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a hash, 32-bit */
static unsigned int fnv1a(const char *s)
{
    unsigned int hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        hash ^= (unsigned int)*p;
        hash *= 16777619u;
    }
    return hash & (VARS_HASH_SIZE - 1);
}

void vars_init(vars_t *v, arena_t *a)
{
    v->scope = NULL;
    v->arena = a;
    vars_push_scope(v);
}

void vars_push_scope(vars_t *v)
{
    var_scope_t *s = arena_alloc(v->arena, sizeof(var_scope_t));
    memset(s->buckets, 0, sizeof(s->buckets));
    s->parent = v->scope;
    v->scope  = s;
}

void vars_pop_scope(vars_t *v)
{
    if (v->scope->parent != NULL)
        v->scope = v->scope->parent;
}

const char *vars_get(vars_t *v, const char *name)
{
    unsigned int idx = fnv1a(name);
    for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
        for (var_entry_t *e = s->buckets[idx]; e != NULL; e = e->next) {
            if (strcmp(e->name, name) == 0)
                return e->value;
        }
    }
    return NULL;
}

/*
 * Search only the current scope for an existing entry.
 * Returns pointer to the entry, or NULL if absent.
 */
static var_entry_t *scope_find(var_scope_t *s, const char *name, unsigned int idx)
{
    for (var_entry_t *e = s->buckets[idx]; e != NULL; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/*
 * Search all scopes (current first, then parents).
 * Returns pointer to the first matching entry, or NULL.
 */
static var_entry_t *vars_find(vars_t *v, const char *name)
{
    unsigned int idx = fnv1a(name);
    for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
        var_entry_t *e = scope_find(s, name, idx);
        if (e)
            return e;
    }
    return NULL;
}

/*
 * Store value in e, reusing e's existing buffer whenever the new value fits.
 *
 * Values live in an arena, and an arena has no per-allocation free. Every
 * reassignment used to arena_strdup() a fresh copy and abandon the old one, so
 * the arena grew ~236 bytes per assignment and a loop like
 *
 *     i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done
 *
 * hit the 64 MB arena cap and aborted after ~270k iterations -- i.e. any
 * long-running script, which is exactly the workload this shell targets.
 * Overwriting in place keeps such a loop flat; only a genuinely longer value
 * allocates, and the doubled capacity makes repeated growth ("9" -> "10" ->
 * "100") amortise instead of reallocating on every digit.
 */
static void var_store_value(vars_t *v, var_entry_t *e, const char *value)
{
    size_t need = strlen(value) + 1;
    if (e->value != NULL && e->value_cap >= need) {
        memcpy(e->value, value, need);
    } else {
        size_t cap = need * 2;
        if (cap < need) cap = need;      /* overflow guard */
        e->value     = arena_alloc(v->arena, cap);
        e->value_cap = cap;
        memcpy(e->value, value, need);
    }

    /* Keep the process environment in sync with an already-exported variable.
     *
     * setenv() COPIES its argument, so the environment entry is a snapshot taken
     * at the moment `export` ran. Assigning again updated e->value but never
     * re-synced, so the child of
     *
     *     export CC; CC=clang; make
     *
     * saw the value CC had when it was exported, not the current one -- silex
     * printed E=one where dash prints E=two, for both `export E=one; E=two` and
     * `E=one; export E; E=two`. POSIX requires the current value. Re-syncing on
     * store is the cheapest correct point: it only costs anything for variables
     * that are actually exported.
     */
    if (e->exported)
        setenv(e->name, e->value, 1);
}

int vars_set_context(vars_t *v, const char *name, const char *value, const char *ctx)
{
    unsigned int idx = fnv1a(name);

    /* Search all scopes for existing entry */
    for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
        var_entry_t *e = scope_find(s, name, idx);
        if (e) {
            if (e->readonly) {
                if (ctx)
                    fprintf(stderr, "%s: %s: is read only\n", ctx, name);
                else
                    fprintf(stderr, "silex: %s: readonly variable\n", name);
                return 1;
            }
            var_store_value(v, e, value);
            return 0;
        }
    }

    /* Not found — create in current scope. The search loop above tolerates a
     * NULL scope (it just iterates zero times); this path dereferences it, so
     * guard explicitly rather than deref NULL if there is no scope to set in. */
    if (!v->scope)
        return 1;
    var_entry_t *e    = arena_alloc(v->arena, sizeof(var_entry_t));
    e->name           = arena_strdup(v->arena, name);
    /* arena_alloc does not zero. Every field var_store_value() reads --
     * value, value_cap, exported -- must be initialised BEFORE the store. */
    e->value          = NULL;
    e->value_cap      = 0;
    e->exported       = 0;
    e->readonly       = 0;
    var_store_value(v, e, value);
    e->next           = v->scope->buckets[idx];
    v->scope->buckets[idx] = e;
    return 0;
}

int vars_set(vars_t *v, const char *name, const char *value)
{
    return vars_set_context(v, name, value, NULL);
}

int vars_set_local(vars_t *v, const char *name, const char *value)
{
    unsigned int idx = fnv1a(name);

    /* Search only the current scope for an existing entry */
    var_entry_t *e = scope_find(v->scope, name, idx);
    if (e) {
        if (e->readonly) {
            fprintf(stderr, "silex: %s: readonly variable\n", name);
            return 1;
        }
        var_store_value(v, e, value);
        return 0;
    }

    /* Create in current scope */
    e               = arena_alloc(v->arena, sizeof(var_entry_t));
    e->name         = arena_strdup(v->arena, name);
    /* arena_alloc does not zero: init everything var_store_value reads first. */
    e->value        = NULL;
    e->value_cap    = 0;
    e->exported     = 0;
    e->readonly     = 0;
    var_store_value(v, e, value);
    e->next         = v->scope->buckets[idx];
    v->scope->buckets[idx] = e;
    return 0;
}

int vars_export_context(vars_t *v, const char *name, const char *ctx)
{
    var_entry_t *e = vars_find(v, name);
    if (!e)
        return 1;
    if (e->readonly && ctx) {
        fprintf(stderr, "%s: %s: is read only\n", ctx, name);
        return 1;
    }
    e->exported = 1;
    setenv(name, e->value, 1);
    return 0;
}

int vars_export(vars_t *v, const char *name)
{
    return vars_export_context(v, name, NULL);
}

int vars_readonly(vars_t *v, const char *name)
{
    var_entry_t *e = vars_find(v, name);
    if (!e)
        return 1;
    e->readonly = 1;
    return 0;
}

int vars_unset_context(vars_t *v, const char *name, const char *ctx)
{
    unsigned int idx = fnv1a(name);

    for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
        var_entry_t **pp = &s->buckets[idx];
        while (*pp) {
            var_entry_t *e = *pp;
            if (strcmp(e->name, name) == 0) {
                if (e->readonly) {
                    if (ctx)
                        fprintf(stderr, "%s: %s: is read only\n", ctx, name);
                    else
                        fprintf(stderr, "silex: %s: readonly variable\n", name);
                    return 1;
                }
                *pp = e->next;
                return 0;
            }
            pp = &e->next;
        }
    }
    return 0;
}

int vars_unset(vars_t *v, const char *name)
{
    return vars_unset_context(v, name, NULL);
}

void vars_export_env(vars_t *v)
{
    for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
        for (int i = 0; i < VARS_HASH_SIZE; i++) {
            for (var_entry_t *e = s->buckets[i]; e != NULL; e = e->next) {
                if (e->exported)
                    setenv(e->name, e->value, 1);
            }
        }
    }
}

void vars_import_env(vars_t *v)
{
    extern char **environ;
    if (!environ) return;
    for (int i = 0; environ[i]; i++) {
        const char *entry = environ[i];
        const char *eq = strchr(entry, '=');
        if (!eq) continue;
        size_t nlen = (size_t)(eq - entry);
        char *name = strndup(entry, nlen);
        if (!name) continue;
        /* Only import if it has a valid shell identifier name */
        int valid = (nlen > 0);
        if (valid) {
            unsigned char fc = (unsigned char)name[0];
            if (!(fc == '_' || (fc >= 'A' && fc <= 'Z') || (fc >= 'a' && fc <= 'z')))
                valid = 0;
        }
        if (valid) {
            for (size_t j = 1; j < nlen; j++) {
                unsigned char c = (unsigned char)name[j];
                if (!(c == '_' || (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
                    valid = 0; break;
                }
            }
        }
        if (valid) {
            /* Only import if not already set (don't override IFS etc.) */
            if (!vars_get(v, name)) {
                unsigned int idx = fnv1a(name);
                var_entry_t *e = arena_alloc(v->arena, sizeof(var_entry_t));
                e->name     = arena_strdup(v->arena, name);
                e->value    = arena_strdup(v->arena, eq + 1);
                e->exported = 1;
                e->readonly = 0;
                e->next     = v->scope->buckets[idx];
                v->scope->buckets[idx] = e;
            }
        }
        free(name);
    }
}

void vars_print_exports(vars_t *v)
{
    /* Collect all exported variables from all scopes (current scope shadows parent) */
    /* Use a simple linear scan to collect unique exports */
    for (int i = 0; i < VARS_HASH_SIZE; i++) {
        for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
            for (var_entry_t *e = s->buckets[i]; e != NULL; e = e->next) {
                if (e->exported) {
                    /* Check if this name was already printed from an inner scope */
                    int shadowed = 0;
                    for (var_scope_t *inner = v->scope; inner != s; inner = inner->parent) {
                        unsigned int idx = fnv1a(e->name);
                        for (var_entry_t *check = inner->buckets[idx]; check; check = check->next) {
                            if (strcmp(check->name, e->name) == 0 && check->exported) {
                                shadowed = 1;
                                break;
                            }
                        }
                        if (shadowed) break;
                    }
                    if (!shadowed) {
                        if (e->value && e->value[0] != '\0') {
                            /* Variable has a value: print export NAME=VALUE */
                            /* TODO: shell quote the value properly */
                            printf("export %s='%s'\n", e->name, e->value);
                        } else {
                            /* Variable is exported but unset (or empty): print export NAME */
                            printf("export %s\n", e->name);
                        }
                    }
                }
            }
        }
    }
}
