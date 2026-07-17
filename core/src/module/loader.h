#ifndef SILEX_LOADER_H
#define SILEX_LOADER_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../../silex_module.h"

/* Load a module .so, performing security checks. Returns NULL on failure. */
silex_module_t *module_load(const char *so_path);

/* Unload a module */
void module_unload(silex_module_t *mod);

#endif /* SILEX_LOADER_H */
