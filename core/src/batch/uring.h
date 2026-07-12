#ifndef SILEX_URING_H
#define SILEX_URING_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stddef.h>

/* Opaque batch operation list */
typedef struct batch_op batch_op_t;

typedef enum {
    BATCH_MKDIR,
    BATCH_CHMOD,
    BATCH_TOUCH,
    BATCH_RM_FILE,
} batch_op_type_t;

typedef struct batch_op {
    batch_op_type_t  type;
    char            *path;   /* heap-allocated */
    unsigned int     mode;   /* for mkdir/chmod */
    struct batch_op *next;
} batch_op_t;

batch_op_t *batch_op_new(batch_op_type_t type, const char *path, unsigned mode);
void        batch_op_free(batch_op_t *op);

/* Execute list of independent operations.
   If io_uring available, use batching. Otherwise sequential. */
int batch_exec(batch_op_t *ops);

#endif /* SILEX_URING_H */
