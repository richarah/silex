/* loader.c — dynamic module loader (dlopen/dlsym) */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "loader.h"
#include "../../matchbox_module.h"

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
 * ABI mismatches.  The libc tag in matchbox_module_t is verified at load
 * time in addition to this directory separation. */
#ifdef MATCHBOX_LIBC_MUSL
#  define MATCHBOX_MODULE_DIR "/usr/lib/matchbox/modules-musl"
#else
#  define MATCHBOX_MODULE_DIR "/usr/lib/matchbox/modules"
#endif

/* Expected libc tag for modules loaded by this build */
#ifdef MATCHBOX_LIBC_MUSL
#  define EXPECTED_LIBC "musl"
#else
#  define EXPECTED_LIBC "glibc"
#endif

/*
 * loader.c — secure .so loading for matchbox modules.
 *
 * Security checks performed before dlopen():
 *  1. lstat() path: must exist and not be a symlink
 *  2. stat() path: dev/ino must match lstat result (race-free check)
 *  3. Owner must be root (uid 0) or the current user
 *  4. File must not be world-writable
 *  5. Containing directory: same ownership and world-writable checks
 *
 * After dlopen():
 *  6. dlsym() for "matchbox_module_init"
 *  7. Call init, verify API version
 *  8. Verify libc tag matches EXPECTED_LIBC
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

matchbox_module_t *module_load(const char *so_path)
{
    void *handle = NULL;
    uid_t my_uid = getuid();

    /* Step 1: lstat() the .so path */
    struct stat lst;
    if (lstat(so_path, &lst) != 0) {
        fprintf(stderr, "matchbox: module_load: lstat '%s': %s\n",
                so_path, strerror(errno));
        return NULL;
    }

    /* Step 2: Reject symlinks */
    if (S_ISLNK(lst.st_mode)) {
        fprintf(stderr, "matchbox: module_load: '%s': module path is a symlink\n",
                so_path);
        return NULL;
    }

    /* Step 3: stat() and compare dev/ino to detect TOCTOU races */
    struct stat st;
    if (stat(so_path, &st) != 0) {
        fprintf(stderr, "matchbox: module_load: stat '%s': %s\n",
                so_path, strerror(errno));
        return NULL;
    }
    if (st.st_dev != lst.st_dev || st.st_ino != lst.st_ino) {
        fprintf(stderr, "matchbox: module_load: '%s': "
                "file identity changed between lstat and stat (possible race)\n",
                so_path);
        return NULL;
    }

    /* Step 4: Check ownership: must be owned by root or current user */
    if (st.st_uid != 0 && st.st_uid != my_uid) {
        fprintf(stderr, "matchbox: module_load: '%s': wrong owner "
                "(uid %u, expected 0 or %u)\n",
                so_path, (unsigned)st.st_uid, (unsigned)my_uid);
        return NULL;
    }

    /* Step 5: Reject world-writable files */
    if (st.st_mode & S_IWOTH) {
        fprintf(stderr, "matchbox: module_load: '%s': world-writable\n", so_path);
        return NULL;
    }

    /* Step 6: Check containing directory */
    char dir_buf[PATH_MAX];
    dir_of(so_path, dir_buf);

    struct stat dir_lst;
    if (lstat(dir_buf, &dir_lst) != 0) {
        fprintf(stderr, "matchbox: module_load: lstat dir '%s': %s\n",
                dir_buf, strerror(errno));
        return NULL;
    }
    if (dir_lst.st_uid != 0 && dir_lst.st_uid != my_uid) {
        fprintf(stderr, "matchbox: module_load: dir '%s': wrong owner "
                "(uid %u, expected 0 or %u)\n",
                dir_buf, (unsigned)dir_lst.st_uid, (unsigned)my_uid);
        return NULL;
    }
    if (dir_lst.st_mode & S_IWOTH) {
        fprintf(stderr, "matchbox: module_load: dir '%s': world-writable\n",
                dir_buf);
        return NULL;
    }

    /* Step 7: dlopen */
    handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "matchbox: module_load: dlopen '%s': %s\n",
                so_path, dlerror());
        return NULL;
    }

    /* Step 8: dlsym for the init function */
    /* Clear any existing error */
    dlerror();
    typedef matchbox_module_t *(*init_fn_t)(void);
    init_fn_t init_fn = (init_fn_t)(uintptr_t)dlsym(handle, "matchbox_module_init");
    const char *dl_err = dlerror();
    if (dl_err || !init_fn) {
        fprintf(stderr, "matchbox: module_load: dlsym '%s' matchbox_module_init: %s\n",
                so_path, dl_err ? dl_err : "symbol not found");
        dlclose(handle);
        return NULL;
    }

    /* Step 9: Call init function */
    matchbox_module_t *mod = init_fn();
    if (!mod) {
        fprintf(stderr, "matchbox: module_load: '%s': matchbox_module_init returned NULL\n",
                so_path);
        dlclose(handle);
        return NULL;
    }

    /* Step 10: Verify API version */
    if (mod->api_version != MATCHBOX_MODULE_API_VERSION) {
        fprintf(stderr, "matchbox: module_load: '%s': API version mismatch "
                "(module %d, runtime %d)\n",
                so_path, mod->api_version, MATCHBOX_MODULE_API_VERSION);
        dlclose(handle);
        return NULL;
    }

    /* Step 11: Verify libc tag -- reject modules built against the wrong libc */
    if (!mod->libc || strcmp(mod->libc, EXPECTED_LIBC) != 0) {
        fprintf(stderr, "matchbox: module_load: '%s': libc mismatch "
                "(got '%s', need '%s')\n",
                so_path,
                mod->libc ? mod->libc : "(null)",
                EXPECTED_LIBC);
        dlclose(handle);
        return NULL;
    }

    return mod;
}

void module_unload(matchbox_module_t *mod)
{
    /* We do not store the dlopen handle in matchbox_module_t directly.
     * In a production system the handle would be stored alongside the module.
     * For now this is a no-op stub: the process lifetime matches module lifetime. */
    (void)mod;
}
