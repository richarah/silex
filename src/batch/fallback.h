#ifndef MATCHBOX_FALLBACK_H
#define MATCHBOX_FALLBACK_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "uring.h"

/* Execute batch operations sequentially without io_uring. */
int fallback_exec_seq(batch_op_t *ops);

#endif /* MATCHBOX_FALLBACK_H */
