/* silex-nosync.c — LD_PRELOAD shim to suppress fsync/fdatasync/sync/msync
 *
 * Loaded by the apt-shim during apk add to avoid disk-sync stalls on every
 * package write. apk is already atomic via rename(2); the syncs are redundant
 * inside a container build layer.
 *
 * Build:
 *   gcc -O2 -shared -fPIC -nostartfiles -o silex-nosync.so silex-nosync.c
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024 silex contributors. Written from scratch; no GPL code.
 */

#include <stddef.h>

int fsync(int fd)                                    { (void)fd; return 0; }
int fdatasync(int fd)                                { (void)fd; return 0; }
void sync(void)                                      {}
int msync(void *addr, size_t length, int flags)      { (void)addr; (void)length; (void)flags; return 0; }
