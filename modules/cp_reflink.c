#define _POSIX_C_SOURCE 200809L

/*
 * cp_reflink.c — silex module: cp --reflink[=auto|always|never]
 *
 * On Linux, tries ioctl(FICLONE) for a copy-on-write reflink clone.
 * Falls back to copy_file_range() then a plain read/write loop.
 *
 * --reflink=auto  (default): try reflink, silently fall back on failure
 * --reflink=always:          error if reflink is not supported by the fs
 * --reflink=never:           skip reflink and use plain copy
 */

#include "../silex_module.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * copy_file_range(2) is a Linux-specific syscall introduced in kernel 4.5.
 * It is not declared by strict POSIX headers; declare it explicitly.
 */
extern ssize_t copy_file_range(int fd_in, off_t *off_in,
                                int fd_out, off_t *off_out,
                                size_t len, unsigned int flags);

/* linux/fs.h may not be present everywhere; define FICLONE manually if absent */
#ifndef FICLONE
#  include <sys/ioctl.h>
#  define FICLONE  _IOW(0x94, 9, int)
#endif

typedef enum {
    REFLINK_AUTO   = 0,
    REFLINK_ALWAYS = 1,
    REFLINK_NEVER  = 2
} reflink_mode_t;

/* Parse the reflink mode string.  argv[flag_index] is the flag itself. */
static reflink_mode_t parse_mode(const char *flag)
{
    /* --reflink          -> auto */
    if (strcmp(flag, "--reflink") == 0)
        return REFLINK_AUTO;
    /* --reflink=auto     -> auto */
    if (strcmp(flag, "--reflink=auto") == 0)
        return REFLINK_AUTO;
    /* --reflink=always   -> always */
    if (strcmp(flag, "--reflink=always") == 0)
        return REFLINK_ALWAYS;
    /* --reflink=never    -> never */
    if (strcmp(flag, "--reflink=never") == 0)
        return REFLINK_NEVER;
    return REFLINK_AUTO;
}

/* Plain read/write copy from src_fd to dst_fd.  Returns 0 on success. */
static int plain_copy(int src_fd, int dst_fd, const char *dst_path)
{
    char buf[65536];
    ssize_t nr;

    while ((nr = read(src_fd, buf, sizeof(buf))) > 0) {
        const char *p  = buf;
        ssize_t remain = nr;
        while (remain > 0) {
            ssize_t nw = write(dst_fd, p, (size_t)remain);
            if (nw < 0) {
                fprintf(stderr, "cp_reflink: write '%s': %s\n",
                        dst_path, strerror(errno));
                return 1;
            }
            p      += nw;
            remain -= nw;
        }
    }
    if (nr < 0) {
        fprintf(stderr, "cp_reflink: read error: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

/*
 * Module handler.
 *
 * Expected argv layout (after applet shift):
 *   argv[0]           = "cp"
 *   argv[flag_index]  = "--reflink[=...]"
 *   remaining args    = other cp flags + SOURCE... DEST
 *
 * We locate SOURCE and DEST as the last two non-flag operands.
 */
static int cp_reflink_handler(int argc, char **argv, int flag_index)
{
    if (argc < 2) {
        fprintf(stderr, "cp_reflink: usage: cp --reflink[=auto|always|never] SOURCE DEST\n");
        return 1;
    }

    reflink_mode_t mode = parse_mode(argv[flag_index]);

    /* Find the last two non-flag arguments as source and destination */
    const char *src  = NULL;
    const char *dst  = NULL;
    int operand_count = 0;

    /* Collect operands (skip flags, skip --reflink* at flag_index) */
    for (int i = 1; i < argc; i++) {
        if (i == flag_index)
            continue;
        if (argv[i][0] == '-' && argv[i][1] != '\0' &&
            strcmp(argv[i], "--") != 0)
            continue;
        if (strcmp(argv[i], "--") == 0)
            continue;
        /* Treat as operand */
        src = dst;
        dst = argv[i];
        operand_count++;
    }

    if (operand_count < 2 || !src || !dst) {
        fprintf(stderr, "cp_reflink: need at least SOURCE and DEST\n");
        return 1;
    }

    /* If --reflink=never, delegate to a plain open/read/write. */
    if (mode == REFLINK_NEVER) {
        int src_fd = open(src, O_RDONLY);
        if (src_fd < 0) {
            fprintf(stderr, "cp_reflink: open '%s': %s\n", src, strerror(errno));
            return 1;
        }
        struct stat ss;
        if (fstat(src_fd, &ss) != 0) {
            fprintf(stderr, "cp_reflink: fstat '%s': %s\n", src, strerror(errno));
            close(src_fd);
            return 1;
        }
        int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, ss.st_mode & 0777);
        if (dst_fd < 0) {
            fprintf(stderr, "cp_reflink: open '%s': %s\n", dst, strerror(errno));
            close(src_fd);
            return 1;
        }
        int ret = plain_copy(src_fd, dst_fd, dst);
        close(src_fd);
        close(dst_fd);
        return ret;
    }

    /* Open source */
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "cp_reflink: open '%s': %s\n", src, strerror(errno));
        return 1;
    }

    struct stat ss;
    if (fstat(src_fd, &ss) != 0) {
        fprintf(stderr, "cp_reflink: fstat '%s': %s\n", src, strerror(errno));
        close(src_fd);
        return 1;
    }

    /* Open destination */
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, ss.st_mode & 0777);
    if (dst_fd < 0) {
        fprintf(stderr, "cp_reflink: open '%s': %s\n", dst, strerror(errno));
        close(src_fd);
        return 1;
    }

    /* Try ioctl FICLONE (copy-on-write clone) */
    int ret = 0;
    if (ioctl(dst_fd, FICLONE, src_fd) == 0) {
        /* Reflink succeeded */
        goto done;
    }

    /* Reflink failed */
    if (mode == REFLINK_ALWAYS) {
        fprintf(stderr, "cp_reflink: '%s' -> '%s': reflink not supported "
                "(filesystem does not support FICLONE)\n", src, dst);
        ret = 1;
        goto done;
    }

    /* mode == REFLINK_AUTO: fall back to copy_file_range then plain copy */
    {
        /* Rewind source */
        if (lseek(src_fd, 0, SEEK_SET) != 0) {
            fprintf(stderr, "cp_reflink: lseek '%s': %s\n", src, strerror(errno));
            ret = 1;
            goto done;
        }
        /* Try copy_file_range (may also fail on old kernels) */
        off_t off_in  = 0;
        off_t off_out = 0;
        ssize_t copied = copy_file_range(src_fd, &off_in,
                                          dst_fd, &off_out,
                                          (size_t)ss.st_size, 0);
        if (copied >= 0 && (off_t)copied == ss.st_size)
            goto done;

        /* Fall back: truncate dst and do plain read/write */
        if (ftruncate(dst_fd, 0) != 0 || lseek(dst_fd, 0, SEEK_SET) != 0 ||
            lseek(src_fd, 0, SEEK_SET) != 0) {
            fprintf(stderr, "cp_reflink: seek/truncate error: %s\n",
                    strerror(errno));
            ret = 1;
            goto done;
        }
        ret = plain_copy(src_fd, dst_fd, dst);
    }

done:
    close(src_fd);
    close(dst_fd);
    return ret;
}

/* Extra flags this module handles */
static const char *cp_reflink_flags[] = {
    "--reflink",
    "--reflink=auto",
    "--reflink=always",
    "--reflink=never",
    NULL
};

static silex_module_t cp_reflink_module = {
    .api_version = SILEX_MODULE_API_VERSION,
    .libc        = SILEX_LIBC_NAME,
    .tool_name   = "cp",
    .module_name = "cp_reflink",
    .description = "cp --reflink: copy-on-write reflink via FICLONE / copy_file_range",
    .extra_flags = cp_reflink_flags,
    .handler     = cp_reflink_handler,
};

silex_module_t *silex_module_init(void)
{
    return &cp_reflink_module;
}
