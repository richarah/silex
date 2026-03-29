/* matchbox_module.h — public module API for matchbox extension modules */

#ifndef MATCHBOX_MODULE_H
#define MATCHBOX_MODULE_H

#include <stddef.h>

#define MATCHBOX_MODULE_API_VERSION 1

typedef struct {
    int api_version;                   /* must be MATCHBOX_MODULE_API_VERSION */
    const char *tool_name;             /* e.g., "cp" */
    const char *module_name;           /* e.g., "cp_gnu_reflink" */
    const char *description;           /* human-readable, for --help */
    const char **extra_flags;          /* NULL-terminated: {"--reflink", "--sparse", NULL} */
    int (*handler)(int argc, char **argv, int flag_index);
    /* flag_index: index into argv where the unrecognised flag was found */
    /* returns: 0 on success, >0 on error (propagated as exit code) */
} matchbox_module_t;

/* Every module .so exports this symbol: */
matchbox_module_t *matchbox_module_init(void);

#endif /* MATCHBOX_MODULE_H */
