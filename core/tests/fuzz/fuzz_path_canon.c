#define _POSIX_C_SOURCE 200809L
/*
 * tests/fuzz/fuzz_path_canon.c — LibFuzzer target for path_canon / path_normalize.
 *
 * Build (requires clang with libFuzzer support):
 *   clang -std=c11 -fsanitize=fuzzer,address,undefined \
 *       -I../../src \
 *       fuzz_path_canon.c \
 *       ../../src/util/path.c \
 *       ../../src/util/error.c \
 *       -o fuzz_path_canon
 *
 * Run:
 *   mkdir -p corpus_path
 *   printf '/usr/local/bin'             > corpus_path/seed0
 *   printf '/tmp/../etc/passwd'         > corpus_path/seed1
 *   printf '../../escape/attempt'       > corpus_path/seed2
 *   printf '////multiple////slashes////' > corpus_path/seed3
 *   printf '/a/b/c/d/e/f/../../../../..' > corpus_path/seed4
 *   ./fuzz_path_canon corpus_path -max_len=4096
 *
 * The fuzzer verifies:
 *   - path_normalize never writes beyond PATH_MAX bytes.
 *   - path_canon never crashes (it may legitimately return NULL for
 *     non-existent paths, which is expected behaviour).
 *   - Neither function has memory errors on arbitrary input.
 *
 * Security property checked:
 *   - path_normalize result never starts with a different root than
 *     would be expected (i.e., /../ never escapes above "/").
 */

#include "../../src/util/path.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Null-terminate. */
    char *input = (char *)malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    /* Output buffers — both exactly PATH_MAX bytes. */
    char norm_buf[PATH_MAX];
    char canon_buf[PATH_MAX];

    /*
     * path_normalize: lexical resolution of . and .. components.
     * Must always succeed or return NULL; must never overflow norm_buf.
     */
    char *norm_result = path_normalize(input, norm_buf);
    if (norm_result != NULL) {
        /*
         * Verify the result fits in PATH_MAX (the function contract).
         * If strnlen returns PATH_MAX, there is no NUL terminator within
         * the buffer — that would be a bug.
         */
        size_t norm_len = strnlen(norm_result, PATH_MAX);
        assert(norm_len < PATH_MAX);

        /*
         * Security property: an absolute-looking input starting with '/'
         * must produce a result that also starts with '/'. A result that
         * starts with anything other than '/' after normalising an absolute
         * path would indicate an escaping bug.
         *
         * For relative inputs we do not enforce this.
         */
        if (input[0] == '/' && norm_result[0] != '\0') {
            assert(norm_result[0] == '/');
        }
    }

    /*
     * path_canon: resolves via realpath(3) — may return NULL for
     * non-existent paths, which is normal and not a bug.
     * We just verify it does not crash.
     */
    (void)path_canon(input, canon_buf);

    free(input);
    return 0;
}
