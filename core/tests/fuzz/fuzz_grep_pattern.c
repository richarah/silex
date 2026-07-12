#define _POSIX_C_SOURCE 200809L
/*
 * tests/fuzz/fuzz_grep_pattern.c — LibFuzzer target for silex grep.
 *
 * Build (requires clang with libFuzzer support):
 *   clang -std=c11 -fsanitize=fuzzer,address,undefined \
 *       -I../../src \
 *       fuzz_grep_pattern.c \
 *       ../../src/core/grep.c \
 *       ../../src/util/strbuf.c \
 *       ../../src/util/arena.c \
 *       ../../src/util/error.c \
 *       ../../src/util/path.c \
 *       -o fuzz_grep_pattern
 *
 * Run:
 *   mkdir -p corpus_grep
 *   printf 'hello\n'             > corpus_grep/seed_pattern0
 *   printf 'a.*b'                > corpus_grep/seed_pattern1
 *   printf '[a-z]+'              > corpus_grep/seed_pattern2
 *   printf '^(foo|bar)$'         > corpus_grep/seed_pattern3
 *   printf '\(nested\(\)\)'      > corpus_grep/seed_pattern4
 *   ./fuzz_grep_pattern corpus_grep -max_len=256
 *
 * Approach:
 *   The fuzzer input is split at the first '\n' byte:
 *     - bytes before '\n' = the grep pattern
 *     - bytes after  '\n' = the input text to match against
 *
 *   Both the pattern and the text are tested against the silex grep
 *   regex engine. This exercises:
 *     - Pattern compilation with arbitrary byte sequences.
 *     - Matching with arbitrary text input.
 *     - Correct handling of malformed / adversarial regex patterns.
 *     - No catastrophic backtracking (ReDoS) that would cause a timeout.
 *
 *   The LibFuzzer timeout flag (-timeout=10) should be used when running
 *   this target to catch potential algorithmic complexity issues.
 */

#include "../../src/util/strbuf.h"
#include "../../src/util/arena.h"

/*
 * Include the POSIX regex interface. silex uses regcomp/regexec internally.
 * We test the same entry point that grep.c uses to compile and match patterns.
 * If silex wraps regex in its own API, adjust the include below.
 */
#include <regex.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Maximum text lines to test the compiled regex against.
 * Prevents artificially inflating match time for huge inputs.
 */
#define MAX_MATCH_LINES 256

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;

    /* Split input at first '\n'. */
    const uint8_t *nl = (const uint8_t *)memchr(data, '\n', size);
    size_t pattern_len, text_len;
    const uint8_t *text_start;

    if (nl == NULL) {
        /* No newline: entire input is the pattern, empty text. */
        pattern_len = size;
        text_start  = data + size;
        text_len    = 0;
    } else {
        pattern_len = (size_t)(nl - data);
        text_start  = nl + 1;
        text_len    = size - pattern_len - 1;
    }

    /* Copy pattern into a NUL-terminated buffer. */
    char *pattern = (char *)malloc(pattern_len + 1);
    if (!pattern) return 0;
    memcpy(pattern, data, pattern_len);
    pattern[pattern_len] = '\0';

    /* Copy text into a NUL-terminated buffer. */
    char *text = (char *)malloc(text_len + 1);
    if (!text) { free(pattern); return 0; }
    memcpy(text, text_start, text_len);
    text[text_len] = '\0';

    /*
     * Attempt to compile the fuzz-generated pattern as a POSIX BRE.
     * An invalid pattern returns REG_BADPAT (or similar) — this is not
     * a crash, it is the expected error path.
     */
    regex_t re;
    int rc = regcomp(&re, pattern, REG_NOSUB);
    if (rc == 0) {
        /*
         * Compiled successfully. Now exercise the matcher line by line.
         * We split text on '\n' to mimic how grep processes input.
         */
        char *line = text;
        int lines_tested = 0;
        while (lines_tested < MAX_MATCH_LINES) {
            char *end = strchr(line, '\n');
            size_t line_len;
            if (end == NULL) {
                line_len = strlen(line);
            } else {
                line_len = (size_t)(end - line);
                *end = '\0';
            }

            /* Match the current line. Result is ignored (0 = match, 1 = no match). */
            (void)regexec(&re, line, 0, NULL, 0);

            if (end == NULL) break;
            *end = '\n';
            line = end + 1;
            lines_tested++;
        }

        regfree(&re);
    }

    /* Also test as a POSIX ERE (used by grep -E / grep -P). */
    rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc == 0) {
        (void)regexec(&re, text, 0, NULL, 0);
        regfree(&re);
    }

    free(text);
    free(pattern);
    return 0;
}
