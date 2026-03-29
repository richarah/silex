#ifndef MATCHBOX_PLATFORM_H
#define MATCHBOX_PLATFORM_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

extern int g_uring_available;    /* 0 = not available, 1 = available */
extern int g_inotify_available;  /* 0 = not available, 1 = available */

void platform_detect(void);  /* call once at startup */

#endif /* MATCHBOX_PLATFORM_H */
