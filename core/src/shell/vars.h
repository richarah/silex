/* vars.h — shell variable store */
#ifndef SILEX_VARS_H
#define SILEX_VARS_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/arena.h"

#define VARS_HASH_SIZE 256   /* power of two */

typedef struct var_entry {
    char             *name;
    char             *value;
    size_t            value_cap;  /* bytes allocated at value, including NUL */
    int               exported;
    int               readonly;
    arena_t          *arena;      /* arena backing name/value (its scope's arena) */
    struct var_entry *next;
} var_entry_t;

typedef struct var_scope {
    var_entry_t      *buckets[VARS_HASH_SIZE];
    struct var_scope *parent;
    /* Arena for entries created in THIS scope. A function-call scope owns a
     * private arena (`own`) that vars_pop_scope frees, so per-call locals and
     * the scope struct are reclaimed on return instead of piling up forever.
     * The global scope points `arena` at the shell's persistent arena and sets
     * has_own = 0 so its variables live for the life of the shell. */
    arena_t          *arena;
    arena_t           own;
    int               has_own;
} var_scope_t;

typedef struct {
    var_scope_t *scope;
    arena_t     *arena;
} vars_t;

void         vars_init(vars_t *v, arena_t *a);
void         vars_push_scope(vars_t *v);
void         vars_pop_scope(vars_t *v);
const char  *vars_get(vars_t *v, const char *name);
int          vars_set(vars_t *v, const char *name, const char *value);
int          vars_set_context(vars_t *v, const char *name, const char *value, const char *ctx);
int          vars_set_local(vars_t *v, const char *name, const char *value);
int          vars_export(vars_t *v, const char *name);
/* Clear the export flag and drop NAME from `environ`; the variable itself
 * keeps its value. Used to undo the temporary export a `VAR=val cmd` prefix
 * needs in order to reach an in-process applet's environment. */
void         vars_unexport(vars_t *v, const char *name);
/* 1 if NAME has an entry with the export flag (even declared-but-unset). */
int          vars_is_exported(vars_t *v, const char *name);
int          vars_export_context(vars_t *v, const char *name, const char *ctx);
int          vars_readonly(vars_t *v, const char *name);
int          vars_is_readonly(vars_t *v, const char *name);  /* query, no diagnostic */
int          vars_unset(vars_t *v, const char *name);
int          vars_unset_context(vars_t *v, const char *name, const char *ctx);
void         vars_export_env(vars_t *v);    /* call setenv for all exported vars */
void         vars_import_env(vars_t *v);    /* import all environ vars as exported */
void         vars_print_exports(vars_t *v); /* print `export` declarations for all exported vars */
void         vars_print_all(vars_t *v);     /* print all vars as NAME='value' (POSIX `set` no-args) */
void         vars_print_readonly(vars_t *v); /* print `readonly` declarations (POSIX `readonly -p`) */

#endif /* SILEX_VARS_H */
