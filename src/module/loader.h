#ifndef MATCHBOX_LOADER_H
#define MATCHBOX_LOADER_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../../matchbox_module.h"

/* Load a module .so, performing security checks. Returns NULL on failure. */
matchbox_module_t *module_load(const char *so_path);

/* Unload a module */
void module_unload(matchbox_module_t *mod);

#endif /* MATCHBOX_LOADER_H */
