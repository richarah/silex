/* path.c — path canonicalisation and validation utilities */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "path.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

char *path_canon(const char *path, char dst[PATH_MAX])
{
    return realpath(path, dst);
}

/*
 * path_normalize: lexically resolve "." and ".." without requiring
 * the path to exist.  Handles both absolute and relative paths.
 */
char *path_normalize(const char *path, char dst[PATH_MAX])
{
    char tmp[PATH_MAX];
    const char *src = path;
    char *out = tmp;
    char *end = tmp + PATH_MAX - 1;

    /* Handle absolute paths */
    if (*src == '/') {
        *out++ = '/';
        src++;
    }

    while (*src) {
        /* Skip multiple slashes */
        while (*src == '/')
            src++;
        if (!*src)
            break;

        /* Find end of this component */
        const char *comp = src;
        while (*src && *src != '/')
            src++;
        size_t clen = (size_t)(src - comp);

        if (clen == 1 && comp[0] == '.') {
            /* "." — skip */
            continue;
        } else if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            /* ".." — back up one component */
            if (out > tmp + 1 || (out == tmp + 1 && tmp[0] == '/')) {
                /* Move back past last slash */
                if (out > tmp && *(out - 1) == '/')
                    out--;
                while (out > tmp && *(out - 1) != '/')
                    out--;
                /* For absolute paths, keep the leading slash */
                if (out == tmp && tmp[0] == '/')
                    out = tmp + 1;
            }
            /* For relative paths at root, ".." is a no-op */
        } else {
            /* Normal component */
            /* Add slash separator if needed */
            if (out > tmp && *(out - 1) != '/') {
                if (out >= end)
                    return NULL;
                *out++ = '/';
            }
            if (out + clen > end)
                return NULL;
            memcpy(out, comp, clen);
            out += clen;
        }
    }

    /* Remove trailing slash unless it's the root */
    if (out > tmp + 1 && *(out - 1) == '/')
        out--;

    *out = '\0';

    if (tmp[0] == '\0') {
        dst[0] = '.';
        dst[1] = '\0';
    } else {
        memcpy(dst, tmp, (size_t)(out - tmp) + 1);
    }

    return dst;
}

const char *path_basename(const char *path)
{
    const char *last = strrchr(path, '/');
    if (!last)
        return path;
    /* Handle trailing slash: "foo/" → "foo" would be odd; return "" */
    if (last[1] == '\0') {
        /* Scan back past trailing slashes */
        while (last > path && *(last - 1) == '/')
            last--;
        if (last == path)
            return "/";
        /* Find the component before the trailing slashes */
        const char *p = last - 1;
        while (p > path && *(p - 1) != '/')
            p--;
        return p;
    }
    return last + 1;
}

char *path_dirname(const char *path, char dst[PATH_MAX])
{
    size_t len = strlen(path);

    if (len == 0) {
        dst[0] = '.';
        dst[1] = '\0';
        return dst;
    }

    /* Copy path so we can modify */
    char tmp[PATH_MAX];
    if (len >= PATH_MAX)
        len = PATH_MAX - 1;
    memcpy(tmp, path, len);
    tmp[len] = '\0';

    /* Strip trailing slashes */
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    /* Find last slash */
    char *slash = strrchr(tmp, '/');
    if (!slash) {
        dst[0] = '.';
        dst[1] = '\0';
        return dst;
    }

    if (slash == tmp) {
        dst[0] = '/';
        dst[1] = '\0';
        return dst;
    }

    *slash = '\0';
    memcpy(dst, tmp, (size_t)(slash - tmp) + 1);
    return dst;
}

char *path_join(const char *dir, const char *base, char dst[PATH_MAX])
{
    size_t dlen = strlen(dir);
    size_t blen = strlen(base);

    /* Skip leading slashes in base if dir is not empty */
    while (*base == '/' && dlen > 0)
        base++, blen--;

    /* dir + '/' + base + NUL */
    if (dlen + 1 + blen + 1 > PATH_MAX)
        return NULL;

    memcpy(dst, dir, dlen);
    if (dlen > 0 && dir[dlen - 1] != '/') {
        dst[dlen++] = '/';
    }
    memcpy(dst + dlen, base, blen + 1);
    return dst;
}
