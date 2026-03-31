#ifndef SILEX_REGISTRY_H
#define SILEX_REGISTRY_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "loader.h"

/* Find a loaded module for (tool, flag). Returns NULL if not found. */
silex_module_t *registry_find(const char *tool, const char *flag);

/* Register a module. */
void registry_register(const char *tool, const char *flag,
                        const char *so_path, silex_module_t *mod);

/* Scan module directory for modules supporting (tool, flag).
   Returns module if found, NULL otherwise. */
silex_module_t *registry_lookup(const char *tool, const char *flag);

/* Invalidate all entries if module directory mtime changed. */
void registry_check_invalidate(void);

#endif /* SILEX_REGISTRY_H */
