/* error.h — error reporting utilities */

#ifndef MATCHBOX_ERROR_H
#define MATCHBOX_ERROR_H

/* Print "matchbox: NAME: MESSAGE\n" to stderr. */
void err_msg(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Like err_msg but appends ": strerror(errno)". */
void err_sys(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Print usage hint to stderr; used when a flag is unrecognised. */
void err_usage(const char *name, const char *usage);

#endif /* MATCHBOX_ERROR_H */
