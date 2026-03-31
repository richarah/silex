/* path.h — path canonicalisation and validation utilities */

#ifndef SILEX_PATH_H
#define SILEX_PATH_H

/* Enable POSIX definitions (PATH_MAX, realpath, etc.) */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>

/* Canonicalise an existing path using realpath(3).
 * dst must be at least PATH_MAX bytes.
 * Returns dst on success, NULL on failure (sets errno). */
char *path_canon(const char *path, char dst[PATH_MAX]);

/* Normalise a path that may not exist yet: resolve "." and ".."
 * components lexically, collapse multiple slashes.
 * dst must be at least PATH_MAX bytes.
 * Returns dst on success, NULL if the result would overflow PATH_MAX. */
char *path_normalize(const char *path, char dst[PATH_MAX]);

/* Return the basename component of path (points into path, no copy). */
const char *path_basename(const char *path);

/* Return the dirname of path into dst (at most PATH_MAX bytes).
 * Returns dst. */
char *path_dirname(const char *path, char dst[PATH_MAX]);

/* Join two path components into dst (at most PATH_MAX bytes).
 * Returns dst on success, NULL if result would overflow. */
char *path_join(const char *dir, const char *base, char dst[PATH_MAX]);

#endif /* SILEX_PATH_H */
