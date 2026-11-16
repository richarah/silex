/*
 * tests/fuzz/fuzz_sed_expr.c -- libFuzzer target for the REAL sed applet.
 *
 * This used to be a stub: it never called sed.c at all, only libc regcomp(),
 * behind a TODO to "replace this stub with the real sed internal API". It
 * now drives applet_sed() exactly the way the shell does when sed runs as a
 * builtin -- same entry point, same process, over and over -- so it covers:
 *
 *   - the shared option parser (src/util/opt.c). Every flag is passed as an
 *     argv word and parsed by opt_init()/opt_next(), in every spelling the
 *     grammar has: separate short words (-n -E), clusters (-nE), long
 *     options (--quiet), joined values (-eSCRIPT, --expression=SCRIPT),
 *     separate values (--expression SCRIPT, -l 5), a positional script, and
 *     a "--" terminator;
 *   - the script compiler (addresses, every command, s flags, y, escapes,
 *     the error paths), from -e segments and from -f FILE;
 *   - the executor, over input bytes taken from the fuzz input;
 *   - cleanup. libFuzzer's LeakSanitizer flags any input after which memory
 *     is still allocated, and the harness aborts if an input leaves a file
 *     descriptor open -- the two bugs a long-lived shell accumulates and a
 *     one-shot process hides (sed leaked a parsed script per rejected
 *     program and an fd per failed `sed -i`).
 *
 * Input layout
 *   byte 0   flags A: 1 -n | 2 -E | 4 -z | 8 -s | 16 --posix | 32 --debug |
 *                     64 -u | 128 --follow-symlinks
 *   byte 1   flags B: bits 0-1 option spelling (0 separate short words,
 *                     1 one short cluster, 2 long, 3 long with =VALUE);
 *                     bits 2-3 script delivery (0 "-e S", 1 "-eS",
 *                     2 --expression, 3 first segment as the positional
 *                     script); 16 last segment via -f FILE; 32 "--" before
 *                     the operands; 64 -l / --line-length from byte 2;
 *                     128 the input file is given twice
 *   byte 2   line length for -l
 *   rest     SCRIPT [ 0x00 INPUT ]   SCRIPT is split into -e segments at
 *            0x01 bytes (at most 16); INPUT is the text sed reads.
 *
 * --sandbox is always passed: fuzz input must not be able to run commands
 * (e, s///e) or write files (w, s///w). Those commands are still parsed --
 * up to the sandbox rejection, which is itself an error path worth fuzzing.
 * -i is never passed for the same reason.
 *
 * The harness is built with -DSILEX_FUZZ, which gives sed.c an execution
 * budget (see fuzz_budget() there): `:a;ba` and `H;G` are valid sed that
 * never terminate or grow without bound, and must not end the campaign.
 *
 * Scripts with stacked regex quantifiers (`a++++`, `\(a*\)*\{9\}...`) or
 * large interval bounds are SKIPPED (see regex_too_costly()). glibc's
 * regcomp() -- which sed uses, and GNU sed's bundled gnulib copy shares --
 * expands each stacked quantifier by duplicating the subtree, so compile
 * time and memory double per operator: GNU sed itself takes ~1 s at 20
 * `+`s and ~5x more per two added. The first campaign found exactly that
 * (`/a+++++++++++++++++++++++++/` -> 2 GB) within three minutes and stopped.
 * It is a property of the C library, not a silex defect, and fuzzing it
 * again finds nothing new.
 *
 * Build and run (make fuzz builds it; seeds in tests/fuzz/corpus/sed/):
 *   make fuzz
 *   mkdir -p build/fuzz/corpus_sed
 *   build/fuzz/fuzz_sed_expr build/fuzz/corpus_sed tests/fuzz/corpus/sed \
 *       -max_len=4096 -timeout=10
 */

#include "../../src/applets.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SEGS   16
#define MAX_ARGV   (8 + 2 * MAX_SEGS + 16)

static char g_in_path[64];
static char g_scr_path[64];
static FILE *g_devnull;

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static int write_file(const char *path, const uint8_t *p, size_t n)
{
    int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0600);
    if (fd < 0)
        return -1;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { close(fd); return -1; }
        p += w;
        n -= (size_t)w;
    }
    close(fd);
    return 0;
}

/* See the header: inputs whose regexes glibc compiles in exponential time
 * and space. A run of 4+ quantifiers (backslashes ignored, so BRE `\+\+`
 * counts), or interval bounds whose product exceeds 4096 (stacked or
 * nested intervals multiply: `a{200}{200}` is 40000 copies of `a`). */
static int regex_too_costly(const char *s, size_t n)
{
    int run = 0;
    unsigned long product = 1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\')
            continue;
        if (c == '*' || c == '+' || c == '?' || c == '}') {
            if (++run >= 4)
                return 1;
        } else {
            run = 0;
        }
        if (c == '{') {
            unsigned long v = 0, hi = 1;
            for (size_t j = i + 1; j < n && j < i + 16; j++) {
                if (s[j] >= '0' && s[j] <= '9') {
                    v = v * 10 + (unsigned long)(s[j] - '0');
                    if (v > 4096)
                        return 1;
                    if (v > hi)
                        hi = v;
                } else if (s[j] == ',') {
                    v = 0;
                } else if (s[j] != '\\') {
                    break;
                }
            }
            product *= hi;
            if (product > 4096)
                return 1;
        }
    }
    return 0;
}

static void cleanup_files(void)
{
    unlink(g_in_path);
    unlink(g_scr_path);
}

/* Undefined behaviour is a finding: stop and keep the input. UBSan's default
 * is to print and carry on, which is how this harness's first UB report
 * (signed overflow in `addr,+N`) produced no reproducer and failed nothing.
 * An explicit UBSAN_OPTIONS still takes precedence. */
const char *__ubsan_default_options(void);
const char *__ubsan_default_options(void)
{
    return "halt_on_error=1:print_stacktrace=1";
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_in_path, sizeof(g_in_path), "%s/fzsedXXXXXX", tmp);
    snprintf(g_scr_path, sizeof(g_scr_path), "%s/fzscrXXXXXX", tmp);
    int a = mkstemp(g_in_path), b = mkstemp(g_scr_path);
    if (a < 0 || b < 0) {
        perror("fuzz_sed_expr: mkstemp");
        abort();
    }
    close(a);
    close(b);
    atexit(cleanup_files);

    /* sed's output goes nowhere. libFuzzer reports on stderr, so only
     * stdout is redirected at the descriptor level. */
    int nul = open("/dev/null", O_WRONLY);
    if (nul >= 0) { dup2(nul, STDOUT_FILENO); close(nul); }
    /* sed's diagnostics are silenced at the stdio level (sanitizer reports
     * write to fd 2 directly and are unaffected). glibc only: musl's
     * stderr is a const pointer. */
    g_devnull = fopen("/dev/null", "w");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3)
        return 0;
    uint8_t fa = data[0], fb = data[1], llen = data[2];
    data += 3;
    size -= 3;

    /* SCRIPT [\0 INPUT] */
    const uint8_t *nul = memchr(data, 0, size);
    size_t slen = nul ? (size_t)(nul - data) : size;
    const uint8_t *in = nul ? nul + 1 : data + size;
    size_t inlen = nul ? size - slen - 1 : 0;

    if (regex_too_costly((const char *)data, slen))
        return 0;

    if (write_file(g_in_path, in, inlen) != 0)
        return 0;

    /* Split the script into -e segments at 0x01. One heap block holds all
     * of them NUL-terminated; argv points into it. */
    char *sbuf = malloc(slen + 1);
    if (!sbuf)
        return 0;
    memcpy(sbuf, data, slen);
    sbuf[slen] = '\0';
    char *segs[MAX_SEGS];
    int nsegs = 0;
    for (char *p = sbuf; nsegs < MAX_SEGS; ) {
        segs[nsegs++] = p;
        char *q = memchr(p, '\x01', (size_t)(sbuf + slen - p));
        if (!q) break;
        *q = '\0';
        p = q + 1;
    }

    int spelling = fb & 3, delivery = (fb >> 2) & 3;
    int use_file = (fb & 16) != 0;
    if (use_file) {
        nsegs--;   /* the last segment becomes the -f script file */
        if (write_file(g_scr_path, (const uint8_t *)segs[nsegs],
                       strlen(segs[nsegs])) != 0) {
            free(sbuf);
            return 0;
        }
    }

    /* Options. Owned strings for the joined/cluster spellings. */
    static const struct { char s; const char *l; } fl[8] = {
        { 'n', "--quiet" }, { 'E', "--regexp-extended" },
        { 'z', "--null-data" }, { 's', "--separate" },
        { 0, "--posix" }, { 0, "--debug" },
        { 'u', "--unbuffered" }, { 0, "--follow-symlinks" },
    };
    char *argv[MAX_ARGV + 1];
    char *owned[MAX_ARGV];
    int argc = 0, nowned = 0;
    char cluster[16];
    char lbuf[32];
    int ci = 0;

    argv[argc++] = (char *)"sed";
    argv[argc++] = (char *)"--sandbox";
    if (spelling == 1)
        cluster[ci++] = '-';
    for (int i = 0; i < 8; i++) {
        if (!(fa & (1u << i)))
            continue;
        if (fl[i].s && spelling == 0) {
            static char shorts[8][3];
            shorts[i][0] = '-'; shorts[i][1] = fl[i].s; shorts[i][2] = '\0';
            argv[argc++] = shorts[i];
        } else if (fl[i].s && spelling == 1) {
            cluster[ci++] = fl[i].s;
        } else {
            argv[argc++] = (char *)fl[i].l;
        }
    }
    if (spelling == 1 && ci > 1) {
        cluster[ci] = '\0';
        argv[argc++] = cluster;
    }
    if (fb & 64) {
        if (spelling >= 2) {
            snprintf(lbuf, sizeof(lbuf), "--line-length=%u", (unsigned)llen);
            argv[argc++] = lbuf;
        } else {
            argv[argc++] = (char *)"-l";
            snprintf(lbuf, sizeof(lbuf), "%u", (unsigned)llen);
            argv[argc++] = lbuf;
        }
    }

    /* Script segments. */
    int positional = (delivery == 3 && nsegs > 0 && !use_file);
    for (int i = positional ? 1 : 0; i < nsegs; i++) {
        switch (delivery) {
        case 1: {                                   /* -eSCRIPT */
            size_t n = strlen(segs[i]);
            char *w = malloc(n + 3);
            if (!w) goto out;
            w[0] = '-'; w[1] = 'e';
            memcpy(w + 2, segs[i], n + 1);
            argv[argc++] = owned[nowned++] = w;
            break;
        }
        case 2:
            if (spelling == 3) {                    /* --expression=SCRIPT */
                size_t n = strlen(segs[i]);
                char *w = malloc(n + 14);
                if (!w) goto out;
                memcpy(w, "--expression=", 13);
                memcpy(w + 13, segs[i], n + 1);
                argv[argc++] = owned[nowned++] = w;
            } else {                                /* --expression SCRIPT */
                argv[argc++] = (char *)"--expression";
                argv[argc++] = segs[i];
            }
            break;
        default:                                    /* -e SCRIPT */
            argv[argc++] = (char *)"-e";
            argv[argc++] = segs[i];
            break;
        }
    }
    if (use_file) {
        argv[argc++] = (char *)(spelling >= 2 ? "--file" : "-f");
        argv[argc++] = g_scr_path;
    }
    if (fb & 32)
        argv[argc++] = (char *)"--";
    if (positional)
        argv[argc++] = segs[0];
    argv[argc++] = g_in_path;
    if (fb & 128)
        argv[argc++] = g_in_path;
    argv[argc] = NULL;

    {
        int fds_before = count_fds();
#ifdef __GLIBC__
        FILE *saved_err = stderr;
        if (g_devnull) stderr = g_devnull;
#endif
        (void)applet_sed(argc, argv);
#ifdef __GLIBC__
        stderr = saved_err;
#endif
        int fds_after = count_fds();
        if (fds_before >= 0 && fds_after != fds_before) {
            fprintf(stderr, "fuzz_sed_expr: file descriptor leak: %d open "
                            "before applet_sed(), %d after\n",
                    fds_before, fds_after);
            abort();
        }
    }

out:
    for (int i = 0; i < nowned; i++)
        free(owned[i]);
    free(sbuf);
    return 0;
}
