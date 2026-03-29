/* vars.h — shell variable store */
#ifndef MATCHBOX_VARS_H
#define MATCHBOX_VARS_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../util/arena.h"

#define VARS_HASH_SIZE 256   /* power of two */

typedef struct var_entry {
    char             *name;
    char             *value;
    int               exported;
    int               readonly;
    struct var_entry *next;
} var_entry_t;

typedef struct var_scope {
    var_entry_t      *buckets[VARS_HASH_SIZE];
    struct var_scope *parent;
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
int          vars_set_local(vars_t *v, const char *name, const char *value);
int          vars_export(vars_t *v, const char *name);
int          vars_readonly(vars_t *v, const char *name);
int          vars_unset(vars_t *v, const char *name);
void         vars_export_env(vars_t *v);   /* call setenv for all exported vars */

#endif /* MATCHBOX_VARS_H */
