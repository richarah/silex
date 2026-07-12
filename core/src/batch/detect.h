#ifndef SILEX_DETECT_H
#define SILEX_DETECT_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "uring.h"

/* Check if a list of operations can be executed independently (safely batched).
   Returns 1 if independent, 0 if must be sequential. */
int batch_ops_independent(batch_op_t *ops);

#endif /* SILEX_DETECT_H */
