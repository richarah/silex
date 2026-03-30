/* matchbox_module.h — public module API for matchbox extension modules */

#ifndef MATCHBOX_MODULE_H
#define MATCHBOX_MODULE_H

#include <stddef.h>

#define MATCHBOX_MODULE_API_VERSION 2

/* MATCHBOX_EXPORT: visibility attribute for module symbols.
 * Apply to matchbox_module_init() in every module .so to ensure the symbol
 * is exported even when the module is compiled with -fvisibility=hidden. */
#if defined(__GNUC__) || defined(__clang__)
#  define MATCHBOX_EXPORT __attribute__((visibility("default")))
#else
#  define MATCHBOX_EXPORT
#endif

/* MATCHBOX_LIBC_NAME: string tag for the libc this translation unit is
 * linked against.  Pass -DMATCHBOX_LIBC_MUSL=1 when building against musl;
 * otherwise defaults to "glibc".  Use in matchbox_module_t.libc. */
#ifdef MATCHBOX_LIBC_MUSL
#  define MATCHBOX_LIBC_NAME "musl"
#else
#  define MATCHBOX_LIBC_NAME "glibc"
#endif

typedef struct {
    int          api_version;   /* must be MATCHBOX_MODULE_API_VERSION */
    const char  *libc;          /* "musl" or "glibc" -- must match host build */
    const char  *tool_name;     /* e.g., "cp" */
    const char  *module_name;   /* e.g., "cp_gnu_reflink" */
    const char  *description;   /* human-readable, for --help */
    const char **extra_flags;   /* NULL-terminated: {"--reflink", "--sparse", NULL} */
    int (*handler)(int argc, char **argv, int flag_index);
    /* flag_index: index into argv where the unrecognised flag was found */
    /* returns: 0 on success, >0 on error (propagated as exit code) */
} matchbox_module_t;

/* Every module .so must export this symbol with MATCHBOX_EXPORT: */
MATCHBOX_EXPORT matchbox_module_t *matchbox_module_init(void);

#endif /* MATCHBOX_MODULE_H */
