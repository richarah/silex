/* loader.c — dynamic module loader (dlopen/dlsym) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "loader.h"
#include "../../silex_module.h"

#include <fcntl.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

/* Module search directory (libc-specific).
 * musl and glibc builds keep modules in separate directories to prevent
 * ABI mismatches.  The libc tag in silex_module_t is verified at load
 * time in addition to this directory separation. */
#ifdef SILEX_LIBC_MUSL
#  define SILEX_MODULE_DIR "/usr/lib/silex/modules-musl"
#else
#  define SILEX_MODULE_DIR "/usr/lib/silex/modules"
#endif

/* Expected libc tag for modules loaded by this build */
#ifdef SILEX_LIBC_MUSL
#  define EXPECTED_LIBC "musl"
#else
#  define EXPECTED_LIBC "glibc"
#endif

/*
 * loader.c — .so loading for silex modules.
 *
 * Every check is anchored to an open file descriptor, and dlopen() is handed
 * that descriptor via /proc/self/fd/N. The bytes we validate are the bytes we
 * load.
 *
 * This used to lstat() the path, stat() it again, compare dev/ino, and call the
 * result a "race-free check". It was not one: dlopen() re-resolves the path from
 * scratch, so the file could be swapped between the final stat() and the
 * dlopen(). Two stats of the same path say nothing about what dlopen will
 * subsequently open. Do not reintroduce a path-based check here.
 *
 * Before dlopen():
 *  1. open(O_RDONLY|O_NOFOLLOW|O_CLOEXEC) -- symlinks are rejected by the kernel
 *  2. fstat(fd): must be a regular file
 *  3. Owner must be root (uid 0) or the current user
 *  4. Must not be group- or world-writable
 *  5. Containing directory: same checks, via its own descriptor (sticky dirs
 *     such as /tmp are permitted)
 *
 * After dlopen():
 *  6. dlsym() for "silex_module_init"
 *  7. Call init, verify API version
 *  8. Verify libc tag matches EXPECTED_LIBC
 *
 * KNOWN GAP: only the immediate parent directory is validated. An attacker who
 * can write an ancestor can replace the whole directory. Walking every ancestor
 * to / is the complete answer.
 */

/* Extract the directory component of a path into dir_buf (PATH_MAX bytes).
 * Returns dir_buf. For a path with no '/', returns ".". */
static char *dir_of(const char *path, char dir_buf[PATH_MAX])
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        dir_buf[0] = '.';
        dir_buf[1] = '\0';
        return dir_buf;
    }
    size_t len = (size_t)(slash - path);
    if (len == 0)
        len = 1; /* root "/" */
    if (len >= PATH_MAX)
        len = PATH_MAX - 1;
    memcpy(dir_buf, path, len);
    dir_buf[len] = '\0';
    return dir_buf;
}

silex_module_t *module_load(const char *so_path)
{
    void *handle = NULL;
    uid_t my_uid = getuid();

    /* The checks below are anchored to an open file descriptor, not to a path.
     *
     * The previous version lstat()ed the path, stat()ed it again, compared
     * dev/ino, and called the result a "race-free check". It was not: dlopen()
     * re-resolves the path from scratch, so an attacker able to write the
     * directory could swap the file between the last stat() and the dlopen()
     * and have an arbitrary .so loaded into the shell. Comparing two stats of
     * the same path tells you nothing about what dlopen will subsequently open.
     *
     * Opening once with O_NOFOLLOW and validating that fd -- then dlopening
     * /proc/self/fd/N -- means the bytes we checked are the bytes we load.
     */
    int fd = open(so_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP) {
            fprintf(stderr, "silex: module_load: '%s': module path is a symlink\n",
                    so_path);
        } else {
            fprintf(stderr, "silex: module_load: open '%s': %s\n",
                    so_path, strerror(errno));
        }
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "silex: module_load: fstat '%s': %s\n",
                so_path, strerror(errno));
        close(fd);
        return NULL;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "silex: module_load: '%s': not a regular file\n", so_path);
        close(fd);
        return NULL;
    }

    /* Ownership: root or the current user. */
    if (st.st_uid != 0 && st.st_uid != my_uid) {
        fprintf(stderr, "silex: module_load: '%s': wrong owner "
                "(uid %u, expected 0 or %u)\n",
                so_path, (unsigned)st.st_uid, (unsigned)my_uid);
        close(fd);
        return NULL;
    }

    /* Reject group- AND world-writable. Only S_IWOTH used to be checked, so a
     * root-owned but group-writable .so -- in a group the attacker belongs to --
     * loaded happily. sudo and ssh reject group-writable for the same reason. */
    if (st.st_mode & (S_IWOTH | S_IWGRP)) {
        fprintf(stderr, "silex: module_load: '%s': %s-writable\n",
                so_path, (st.st_mode & S_IWOTH) ? "world" : "group");
        close(fd);
        return NULL;
    }

    /* The containing directory, likewise via a descriptor.
     *
     * NOTE: only the immediate parent is checked. If an ancestor is writable by
     * an attacker they can replace the whole directory. Walking every ancestor
     * to / is the complete answer; this is not that. */
    char dir_buf[PATH_MAX];
    dir_of(so_path, dir_buf);

    int dfd = open(dir_buf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        fprintf(stderr, "silex: module_load: open dir '%s': %s\n",
                dir_buf, strerror(errno));
        close(fd);
        return NULL;
    }

    struct stat dir_st;
    if (fstat(dfd, &dir_st) != 0) {
        fprintf(stderr, "silex: module_load: fstat dir '%s': %s\n",
                dir_buf, strerror(errno));
        close(dfd);
        close(fd);
        return NULL;
    }
    close(dfd);

    if (dir_st.st_uid != 0 && dir_st.st_uid != my_uid) {
        fprintf(stderr, "silex: module_load: dir '%s': wrong owner "
                "(uid %u, expected 0 or %u)\n",
                dir_buf, (unsigned)dir_st.st_uid, (unsigned)my_uid);
        close(fd);
        return NULL;
    }
    if ((dir_st.st_mode & (S_IWOTH | S_IWGRP)) && !(dir_st.st_mode & S_ISVTX)) {
        /* Sticky (like /tmp) is acceptable: only the owner can remove entries. */
        fprintf(stderr, "silex: module_load: dir '%s': %s-writable\n",
                dir_buf, (dir_st.st_mode & S_IWOTH) ? "world" : "group");
        close(fd);
        return NULL;
    }

    /* dlopen the descriptor we validated, not the path we validated it through.
     * Requires /proc; in a container it is mounted. If it is not, refuse rather
     * than fall back to the racy path-based dlopen. */
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);

    handle = dlopen(fd_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "silex: module_load: dlopen '%s': %s\n",
                so_path, dlerror());
        close(fd);
        return NULL;
    }
    close(fd);

    /* Step 8: dlsym for the init function */
    /* Clear any existing error */
    dlerror();
    typedef silex_module_t *(*init_fn_t)(void);
    init_fn_t init_fn = (init_fn_t)(uintptr_t)dlsym(handle, "silex_module_init");
    const char *dl_err = dlerror();
    if (dl_err || !init_fn) {
        fprintf(stderr, "silex: module_load: dlsym '%s' silex_module_init: %s\n",
                so_path, dl_err ? dl_err : "symbol not found");
        dlclose(handle);
        return NULL;
    }

    /* Step 9: Call init function */
    silex_module_t *mod = init_fn();
    if (!mod) {
        fprintf(stderr, "silex: module_load: '%s': silex_module_init returned NULL\n",
                so_path);
        dlclose(handle);
        return NULL;
    }

    /* Step 10: Verify API version */
    if (mod->api_version != SILEX_MODULE_API_VERSION) {
        fprintf(stderr, "silex: module_load: '%s': API version mismatch "
                "(module %d, runtime %d)\n",
                so_path, mod->api_version, SILEX_MODULE_API_VERSION);
        dlclose(handle);
        return NULL;
    }

    /* Step 11: Verify libc tag -- reject modules built against the wrong libc */
    if (!mod->libc || strcmp(mod->libc, EXPECTED_LIBC) != 0) {
        fprintf(stderr, "silex: module_load: '%s': libc mismatch "
                "(got '%s', need '%s')\n",
                so_path,
                mod->libc ? mod->libc : "(null)",
                EXPECTED_LIBC);
        dlclose(handle);
        return NULL;
    }

    return mod;
}

void module_unload(silex_module_t *mod)
{
    /* We do not store the dlopen handle in silex_module_t directly.
     * In a production system the handle would be stored alongside the module.
     * For now this is a no-op stub: the process lifetime matches module lifetime. */
    (void)mod;
}
