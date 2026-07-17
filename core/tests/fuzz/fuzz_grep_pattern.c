#define _POSIX_C_SOURCE 200809L
/*
 * tests/fuzz/fuzz_grep_pattern.c — LibFuzzer target for silex's regex engine.
 *
 * This harness fuzzes THE ENGINE silex GREP ACTUALLY USES: the Thompson
 * NFA/DFA in src/util/regex/ (mb_regex_compile / mb_regex_search), reached
 * through grep.c's <../util/regex/regex.h>.
 *
 * It previously #included the system <regex.h> and called glibc regcomp() —
 * so it fuzzed libc, not silex. That harness reported an "out-of-memory" on a
 * ~10-byte interval pattern (a{100000000}); that is glibc regcomp expanding the
 * interval, NOT a silex defect. silex's parser approximates {n,m} as '+'
 * (src/util/regex/parse.c:118,159), so it never allocates per-repetition and
 * cannot blow up that way. Testing the wrong engine hid that and manufactured a
 * false crash. Fixed: this harness now exercises the real silex path.
 *
 * Build (requires clang with libFuzzer support):
 *   make CC=clang fuzz     # builds build/fuzz/fuzz_grep_pattern
 *
 * Input is split at the first '\n':
 *   - bytes before '\n' = the pattern (compiled as BRE, then as ERE)
 *   - bytes after  '\n' = the subject text searched with mb_regex_search
 */

#include "../../src/util/regex/regex.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Cap subject length so a huge input cannot inflate search time and be
 * misreported as an algorithmic-complexity finding. */
#define MAX_TEXT 65536

static void exercise(const char *pattern, int flags,
                     const char *text, size_t text_len)
{
    const char *err = NULL;
    mb_regex *re = mb_regex_compile(pattern, flags, &err);
    if (re == NULL) return;                 /* invalid pattern: expected path */
    mb_match m;
    (void)mb_regex_search(re, text, text_len, &m);
    (void)mb_regex_first_char(re);
    mb_regex_free(re);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;

    const uint8_t *nl = (const uint8_t *)memchr(data, '\n', size);
    size_t pattern_len;
    const uint8_t *text_start;
    size_t text_len;

    if (nl == NULL) {
        pattern_len = size;
        text_start  = data + size;
        text_len    = 0;
    } else {
        pattern_len = (size_t)(nl - data);
        text_start  = nl + 1;
        text_len    = size - pattern_len - 1;
    }

    if (text_len > MAX_TEXT) text_len = MAX_TEXT;

    char *pattern = (char *)malloc(pattern_len + 1);
    if (!pattern) return 0;
    memcpy(pattern, data, pattern_len);
    pattern[pattern_len] = '\0';

    char *text = (char *)malloc(text_len + 1);
    if (!text) { free(pattern); return 0; }
    memcpy(text, text_start, text_len);
    text[text_len] = '\0';

    exercise(pattern, SX_REG_BRE, text, text_len);
    exercise(pattern, SX_REG_ERE, text, text_len);

    free(text);
    free(pattern);
    return 0;
}
