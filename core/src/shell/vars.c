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
    v->arena = a;
    /* The global scope lives for the life of the shell; its variables stay in
     * the persistent arena `a`. Allocate the scope struct there too (never
     * freed) and mark has_own = 0 so vars_pop_scope leaves it alone. */
    var_scope_t *g = arena_alloc(a, sizeof(var_scope_t));
    memset(g->buckets, 0, sizeof(g->buckets));
    g->parent  = NULL;
    g->arena   = a;
    g->has_own = 0;
    v->scope   = g;
}

void vars_push_scope(vars_t *v)
{
    /* A function-call scope gets a PRIVATE arena that vars_pop_scope frees, so
     * the scope struct, its locals, and their values are reclaimed on return.
     * Without this, every call leaked a ~2 KB scope (plus locals) into the
     * persistent arena, and a function-calling loop hit the 64 MB cap. The
     * struct is malloc'd (not arena-allocated) so it too is freed on pop. */
    var_scope_t *s = malloc(sizeof(var_scope_t));
    if (!s) {
        perror("silex: vars_push_scope");
        abort();
    }
    memset(s->buckets, 0, sizeof(s->buckets));
    s->parent  = v->scope;
    arena_init(&s->own, "scope");
    s->arena   = &s->own;
    s->has_own = 1;
    v->scope   = s;
}

void vars_pop_scope(vars_t *v)
{
    var_scope_t *s = v->scope;
    if (s->parent == NULL)
        return;                 /* never pop the global scope */
    v->scope = s->parent;
    if (s->has_own) {
        arena_free(&s->own);    /* frees this scope's locals and their values */
        free(s);
    }
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
 * Create a "declared but unset" entry (value == NULL) in the GLOBAL scope and
 * return it. `readonly NAME` and `export NAME` on an as-yet-unset name must
 * record the attribute against a variable that stays UNSET -- distinct from an
 * empty value -- so that `${NAME+x}` still reports it unset while `readonly -p`
 * / `export -p` list it. Placing it in the global scope matches plain
 * assignment (vars_set_context): attributes set inside a function apply to the
 * global variable unless `local` was used. Callers must have confirmed the name
 * is not already present in any scope (via vars_find) before calling.
 */
static var_entry_t *declare_global_unset(vars_t *v, const char *name)
{
    unsigned int idx = fnv1a(name);
    var_scope_t *global = v->scope;
    if (!global)
        return NULL;
    while (global->parent != NULL)
        global = global->parent;
    var_entry_t *e   = arena_alloc(global->arena, sizeof(var_entry_t));
    e->arena         = global->arena;
    e->name          = arena_strdup(global->arena, name);
    e->value         = NULL;   /* declared but unset */
    e->value_cap     = 0;
    e->exported      = 0;
    e->readonly      = 0;
    e->next          = global->buckets[idx];
    global->buckets[idx] = e;
    return e;
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
    (void)v;   /* storage arena now comes from e->arena, not v->arena */
    size_t need = strlen(value) + 1;
    if (e->value != NULL && e->value_cap >= need) {
        memcpy(e->value, value, need);
    } else {
        size_t cap = need * 2;
        if (cap < need) cap = need;      /* overflow guard */
        /* Allocate from the entry's own scope arena (e->arena), not v->arena:
         * a local's storage must be reclaimed when its scope is popped, and a
         * global's must persist. */
        e->value     = arena_alloc(e->arena, cap);
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

/* Diagnostic for an attempt to write or unset a read-only variable. `ctx` is
 * the builtin's name when one is responsible (`readonly`, `export`, `unset`),
 * which shells put in front of the message; NULL for a plain assignment. */
static void readonly_error(const char *ctx, const char *name)
{
    if (!ctx)
        fprintf(stderr, "silex: %s: readonly variable\n", name);
    else if (strcmp(ctx, "unset") == 0)
        /* smoosh's builtin.unset compares stderr byte for byte and expects
         * this exact wording ("unset: x is read-only"), which differs from the
         * assignment/export message below. */
        fprintf(stderr, "%s: %s is read-only\n", ctx, name);
    else
        fprintf(stderr, "%s: %s: is read only\n", ctx, name);
}

int vars_set_context(vars_t *v, const char *name, const char *value, const char *ctx)
{
    unsigned int idx = fnv1a(name);

    /* Search all scopes for existing entry */
    for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
        var_entry_t *e = scope_find(s, name, idx);
        if (e) {
            if (e->readonly) {
                readonly_error(ctx, name);
                return 1;
            }
            var_store_value(v, e, value);
            return 0;
        }
    }

    /* Not found — create it in the GLOBAL (outermost) scope, not the current
     * one. POSIX: a plain assignment inside a function operates on the global
     * variable; only `local` (vars_set_local) creates a function-local. Creating
     * here in v->scope meant `f() { X=1; }; f; echo "$X"` lost X the moment the
     * function's scope was popped -- and, via `MSH_SHELL=$shell` inside its
     * search function, stopped modernish from ever recognising a usable shell.
     *
     * The search loop above already updates a `local` X in an inner scope, so
     * only genuinely-new variables reach here. */
    var_scope_t *global = v->scope;
    if (!global)
        return 1;                       /* no scope to set in */
    while (global->parent != NULL)
        global = global->parent;
    var_entry_t *e    = arena_alloc(global->arena, sizeof(var_entry_t));
    e->arena          = global->arena;
    e->name           = arena_strdup(global->arena, name);
    /* arena_alloc does not zero. Every field var_store_value() reads --
     * value, value_cap, exported -- must be initialised BEFORE the store. */
    e->value          = NULL;
    e->value_cap      = 0;
    e->exported       = 0;
    e->readonly       = 0;
    var_store_value(v, e, value);
    e->next               = global->buckets[idx];
    global->buckets[idx]  = e;
    return 0;
}

int vars_set(vars_t *v, const char *name, const char *value)
{
    return vars_set_context(v, name, value, NULL);
}

int vars_is_local(vars_t *v, const char *name)
{
    return scope_find(v->scope, name, fnv1a(name)) != NULL;
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
    e               = arena_alloc(v->scope->arena, sizeof(var_entry_t));
    e->arena        = v->scope->arena;
    e->name         = arena_strdup(v->scope->arena, name);
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
    if (!e) {
        /* POSIX: `export NAME` on an unset name marks it for export without
         * setting it. It enters the environment only once it is assigned a
         * value (var_store_value re-syncs then). modernish's `isset -x` on an
         * unset name depends on this staying UNSET, not becoming empty. */
        e = declare_global_unset(v, name);
        if (!e)
            return 1;
        e->exported = 1;
        return 0;
    }
    if (e->readonly && ctx) {
        if (strcmp(ctx, "unset") == 0)
            fprintf(stderr, "%s: %s is read-only\n", ctx, name);
        else
            fprintf(stderr, "%s: %s: is read only\n", ctx, name);
        return 1;
    }
    e->exported = 1;
    if (e->value)                 /* keep a declared-but-unset var out of environ */
        setenv(name, e->value, 1);
    return 0;
}

int vars_export(vars_t *v, const char *name)
{
    return vars_export_context(v, name, NULL);
}

void vars_unexport(vars_t *v, const char *name)
{
    var_entry_t *e = vars_find(v, name);
    if (e)
        e->exported = 0;
    unsetenv(name);
}

int vars_is_readonly(vars_t *v, const char *name)
{
    var_entry_t *e = vars_find(v, name);
    return e != NULL && e->readonly;
}

int vars_readonly(vars_t *v, const char *name)
{
    var_entry_t *e = vars_find(v, name);
    if (!e) {
        /* POSIX: `readonly NAME` on an unset name creates a declared-but-unset
         * read-only variable. It stays unset (distinct from empty) until, and
         * unless, it is later assigned -- modernish's `isset -r` on an unset
         * name relies on this and on `readonly -p` listing it. */
        e = declare_global_unset(v, name);
        if (!e)
            return 1;
    }
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
                    readonly_error(ctx, name);
                    return 1;
                }
                *pp = e->next;
                /* Also drop it from the process environment. An exported var
                 * lives in `environ` (via setenv); removing only the shell-table
                 * entry left the stale value visible to children, so
                 * `export V=x; unset V; child` still saw V=x. POSIX unset
                 * removes the variable entirely. This also made modernish
                 * falsely detect BUG_EXPORTUNS, because it reuses `_Msh_test`
                 * (exported with a value, later unset) as a scratch name. */
                unsetenv(name);
                return 0;
            }
            pp = &e->next;
        }
    }
    /* Not in the shell table, but a bare `environ` entry may still linger
     * (e.g. inherited but never imported): clear it too so unset is complete. */
    unsetenv(name);
    return 0;
}

/* A name always lands in the same bucket, so hashing it finds the same entry
 * the old full-table sweep did -- it just skips the 255 buckets that cannot
 * hold it. This is called once per variable assignment in a command, and the
 * sweep made every assignment O(VARS_HASH_SIZE) rather than O(1): it was the
 * single largest self-time cost in the parameter-expansion benchmark. */
int vars_is_exported(vars_t *v, const char *name)
{
    var_entry_t *e = vars_find(v, name);
    return e ? e->exported : 0;
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
                if (e->exported && e->value)  /* skip declared-but-unset exports */
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
                arena_t *ar = v->scope->arena;
                var_entry_t *e = arena_alloc(ar, sizeof(var_entry_t));
                e->arena    = ar;
                e->name     = arena_strdup(ar, name);
                e->value    = arena_strdup(ar, eq + 1);
                /* value_cap must reflect the real allocation: a later
                 * reassignment checks it to decide whether the buffer can be
                 * overwritten in place. Leaving it uninitialised risked an
                 * out-of-bounds memcpy on the next assignment to an imported
                 * env var. */
                e->value_cap = strlen(eq + 1) + 1;
                e->exported = 1;
                e->readonly = 0;
                e->next     = v->scope->buckets[idx];
                v->scope->buckets[idx] = e;
            }
        }
        free(name);
    }
}

/* Print NAME='value' with the value single-quoted for safe re-input: a literal
 * single quote becomes '\'' (close, escaped quote, reopen). */
static void print_squoted_assignment(const char *name, const char *value)
{
    fputs(name, stdout);
    fputs("='", stdout);
    for (const char *p = value; p && *p; p++) {
        if (*p == '\'')
            fputs("'\\''", stdout);
        else
            putchar((unsigned char)*p);
    }
    fputs("'\n", stdout);
}

static int name_cmp(const void *a, const void *b)
{
    const var_entry_t *ea = *(const var_entry_t *const *)a;
    const var_entry_t *eb = *(const var_entry_t *const *)b;
    return strcmp(ea->name, eb->name);
}

/* POSIX `set` with no operands: write every shell variable as NAME='value',
 * sorted by name, in a form suitable for re-input. */
void vars_print_all(vars_t *v)
{
    /* Count, then collect the visible entries (an inner scope shadows outer). */
    size_t cap = 64, n = 0;
    var_entry_t **arr = malloc(cap * sizeof(*arr));
    if (!arr) return;

    for (int i = 0; i < VARS_HASH_SIZE; i++) {
        for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
            for (var_entry_t *e = s->buckets[i]; e != NULL; e = e->next) {
                /* A declared-but-unset variable (value == NULL) is not a set
                 * variable; POSIX `set` lists only set ones, and printing it as
                 * NAME='' would wrongly claim it holds the empty string. */
                if (!e->value) continue;
                int shadowed = 0;
                for (var_scope_t *inner = v->scope; inner != s; inner = inner->parent) {
                    if (scope_find(inner, e->name, (unsigned int)i)) { shadowed = 1; break; }
                }
                if (shadowed) continue;
                if (n == cap) {
                    size_t ncap = cap * 2;
                    var_entry_t **na = realloc(arr, ncap * sizeof(*arr));
                    if (!na) { free(arr); return; }
                    arr = na; cap = ncap;
                }
                arr[n++] = e;
            }
        }
    }

    qsort(arr, n, sizeof(*arr), name_cmp);
    for (size_t i = 0; i < n; i++)
        print_squoted_assignment(arr[i]->name, arr[i]->value ? arr[i]->value : "");
    free(arr);
}

/* POSIX `readonly -p`: list read-only variables as `readonly NAME='value'`
 * (or `readonly NAME` when it has no value), sorted by name. modernish's
 * `isset -r` parses this. */
void vars_print_readonly(vars_t *v)
{
    size_t cap = 32, n = 0;
    var_entry_t **arr = malloc(cap * sizeof(*arr));
    if (!arr) return;
    for (int i = 0; i < VARS_HASH_SIZE; i++) {
        for (var_scope_t *s = v->scope; s != NULL; s = s->parent) {
            for (var_entry_t *e = s->buckets[i]; e != NULL; e = e->next) {
                if (!e->readonly) continue;
                int shadowed = 0;
                for (var_scope_t *inner = v->scope; inner != s; inner = inner->parent)
                    if (scope_find(inner, e->name, (unsigned int)i)) { shadowed = 1; break; }
                if (shadowed) continue;
                if (n == cap) {
                    size_t ncap = cap * 2;
                    var_entry_t **na = realloc(arr, ncap * sizeof(*arr));
                    if (!na) { free(arr); return; }
                    arr = na; cap = ncap;
                }
                arr[n++] = e;
            }
        }
    }
    qsort(arr, n, sizeof(*arr), name_cmp);
    for (size_t i = 0; i < n; i++) {
        if (arr[i]->value && arr[i]->value[0] != '\0') {
            fputs("readonly ", stdout);
            print_squoted_assignment(arr[i]->name, arr[i]->value);
        } else {
            printf("readonly %s\n", arr[i]->name);
        }
    }
    free(arr);
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
                            /* POSIX: the listing must be suitable for reinput.
                             * This printed the value between bare quotes, so
                             * any value CONTAINING a quote came back mangled --
                             * V=a'b'c listed as export V='a'b'c', which
                             * re-reads as abc, and an odd number of quotes made
                             * `eval "$(export -p)"` a syntax error. That is the
                             * standard save/restore idiom; ShellSpec's
                             * shellspec_list_envkeys evals this output. */
                            fputs("export ", stdout);
                            print_squoted_assignment(e->name, e->value);
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
