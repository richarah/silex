/* test_regex.c — unit tests for the silex Thompson NFA/DFA regex engine
 *
 * Compares mb_regex results against libc regexec for correctness.
 * Any difference = bug.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

/* Include the regex engine */
#include "../../src/util/regex/regex.h"

static int g_passed = 0;
static int g_failed = 0;

/* ---- Test helper ---------------------------------------------------------- */

/*
 * Check that mb_regex_search(pat, flags, text) matches iff libc regexec matches.
 * Optionally checks match position equality.
 */
static void check(const char *pat, int mb_flags, int posix_flags,
                  const char *text, const char *label)
{
    /* mb_regex result */
    const char *err = NULL;
    mb_regex *re = mb_regex_compile(pat, mb_flags, &err);
    int mb_result = 0;
    mb_match mb_m = {NULL, NULL};

    if (re) {
        mb_result = mb_regex_search(re, text, strlen(text), &mb_m);
        mb_regex_free(re);
    } else {
        /* Compile failed — check if libc also fails */
    }

    /* POSIX regexec result */
    regex_t posix_re;
    int posix_result = 0;
    regmatch_t pm;
    if (regcomp(&posix_re, pat, posix_flags) == 0) {
        posix_result = (regexec(&posix_re, text, 1, &pm, 0) == 0) ? 1 : 0;
        regfree(&posix_re);
    }

    if (mb_result == posix_result) {
        g_passed++;
        /* printf("  PASS: %s\n", label); */
    } else {
        g_failed++;
        printf("  FAIL: %s\n", label);
        printf("        pat='%s' text='%s'\n", pat, text);
        printf("        mb=%d posix=%d\n", mb_result, posix_result);
    }
}

/* Convenience: BRE test */
static void bre(const char *pat, const char *text, int expect_match,
                const char *label)
{
    (void)expect_match;
    check(pat, SX_REG_BRE, 0, text, label);
}

/* Convenience: ERE test */
static void ere(const char *pat, const char *text, int expect_match,
                const char *label)
{
    (void)expect_match;
    check(pat, SX_REG_ERE, REG_EXTENDED, text, label);
}

/* Convenience: case-insensitive BRE test */
static void bre_i(const char *pat, const char *text, int expect_match,
                  const char *label)
{
    (void)expect_match;
    check(pat, SX_REG_BRE | SX_REG_ICASE, REG_ICASE, text, label);
}

/* ---- Test suites ---------------------------------------------------------- */

static void test_literals(void)
{
    printf("--- Literals ---\n");
    bre("abc", "abcdef", 1, "BRE: literal match at start");
    bre("abc", "xabcdef", 1, "BRE: literal match in middle");
    bre("abc", "xyz", 0, "BRE: literal no match");
    bre("abc", "", 0, "BRE: literal empty text");
    bre("", "abc", 1, "BRE: empty pattern matches");
    bre("", "", 1, "BRE: both empty");
    bre("a", "a", 1, "BRE: single char match");
    bre("a", "b", 0, "BRE: single char no match");
    bre("hello world", "say hello world please", 1, "BRE: multi-word");
}

static void test_anchors(void)
{
    printf("--- Anchors ---\n");
    bre("^abc", "abcdef", 1, "BRE: BOL match");
    bre("^abc", "xabc", 0, "BRE: BOL no match");
    bre("abc$", "xabc", 1, "BRE: EOL match");
    bre("abc$", "abcx", 0, "BRE: EOL no match");
    bre("^abc$", "abc", 1, "BRE: full match");
    bre("^abc$", "abcd", 0, "BRE: full no match (longer)");
    bre("^abc$", "xabc", 0, "BRE: full no match (prefix)");
    bre("^$", "", 1, "BRE: empty anchored match");
    ere("^abc", "abcdef", 1, "ERE: BOL match");
    ere("abc$", "xabc", 1, "ERE: EOL match");
    ere("^abc$", "abc", 1, "ERE: full match");
}

static void test_dot(void)
{
    printf("--- Dot (.) ---\n");
    bre("a.c", "abc", 1, "BRE: dot matches any");
    bre("a.c", "a\tc", 1, "BRE: dot matches tab");
    bre("a.c", "ac", 0, "BRE: dot requires char");
    bre("...", "abc", 1, "BRE: three dots");
    bre("...", "ab", 0, "BRE: three dots too short");
    ere("a.c", "axc", 1, "ERE: dot");
}

static void test_star(void)
{
    printf("--- Star (*) ---\n");
    bre("ab*c", "ac", 1, "BRE: zero b's");
    bre("ab*c", "abc", 1, "BRE: one b");
    bre("ab*c", "abbc", 1, "BRE: two b's");
    bre("ab*c", "abbbbc", 1, "BRE: many b's");
    bre("a*", "", 1, "BRE: a* matches empty");
    bre("a*b", "aaab", 1, "BRE: a* before b");
    bre("a.*b", "aXYZb", 1, "BRE: .* matches middle");
    ere("ab*c", "abbc", 1, "ERE: star");
}

static void test_plus_quest(void)
{
    printf("--- Plus (+) and Question (?) ---\n");
    ere("ab+c", "abc", 1, "ERE: + one");
    ere("ab+c", "abbc", 1, "ERE: + two");
    ere("ab+c", "ac", 0, "ERE: + zero fails");
    ere("ab?c", "abc", 1, "ERE: ? one");
    ere("ab?c", "ac", 1, "ERE: ? zero");
    ere("ab?c", "abbc", 0, "ERE: ? two fails");
    /* BRE \+ and \? (GNU extensions) */
    bre("ab\\+c", "abc", 1, "BRE: \\+ one");
    bre("ab\\+c", "ac", 0, "BRE: \\+ zero fails");
    bre("ab\\?c", "abc", 1, "BRE: \\? one");
    bre("ab\\?c", "ac", 1, "BRE: \\? zero");
}

static void test_alternation(void)
{
    printf("--- Alternation ---\n");
    ere("cat|dog", "I have a cat", 1, "ERE: | first alt");
    ere("cat|dog", "I have a dog", 1, "ERE: | second alt");
    ere("cat|dog", "I have a bird", 0, "ERE: | neither");
    ere("a|b|c", "b", 1, "ERE: multi-alt mid");
    ere("a|b|c", "d", 0, "ERE: multi-alt none");
    /* BRE \| (GNU extension) */
    bre("cat\\|dog", "I have a cat", 1, "BRE: \\| first alt");
    bre("cat\\|dog", "I have a dog", 1, "BRE: \\| second alt");
    bre("error\\|warning\\|fatal", "error: bad thing", 1, "BRE: three-way | match 1");
    bre("error\\|warning\\|fatal", "warning: also bad", 1, "BRE: three-way | match 2");
    bre("error\\|warning\\|fatal", "fatal: die", 1, "BRE: three-way | match 3");
    bre("error\\|warning\\|fatal", "info: fine", 0, "BRE: three-way | no match");
}

static void test_groups(void)
{
    printf("--- Groups ---\n");
    ere("(ab)+", "ababab", 1, "ERE: group+");
    ere("(ab)+", "ac", 0, "ERE: group+ no match");
    ere("(a|b)+", "ababba", 1, "ERE: group with alt");
    ere("(foo|bar)baz", "foobaz", 1, "ERE: group alt + literal");
    ere("(foo|bar)baz", "barbaz", 1, "ERE: group alt + literal 2");
    ere("(foo|bar)baz", "quxbaz", 0, "ERE: group alt + literal no match");
    bre("\\(ab\\)\\+", "ababab", 1, "BRE: group\\+");
    bre("\\(foo\\|bar\\)baz", "foobaz", 1, "BRE: group \\| + literal");
    bre("\\(foo\\|bar\\)baz", "barbaz", 1, "BRE: group \\| + literal 2");
}

static void test_charclasses(void)
{
    printf("--- Character classes ---\n");
    bre("[abc]", "a", 1, "BRE: [abc] a");
    bre("[abc]", "b", 1, "BRE: [abc] b");
    bre("[abc]", "d", 0, "BRE: [abc] d");
    bre("[a-z]", "m", 1, "BRE: [a-z] match");
    bre("[a-z]", "M", 0, "BRE: [a-z] no match upper");
    bre("[0-9]", "5", 1, "BRE: [0-9] match");
    bre("[0-9]", "a", 0, "BRE: [0-9] no match");
    bre("[^abc]", "d", 1, "BRE: [^abc] match");
    bre("[^abc]", "a", 0, "BRE: [^abc] no match");
    bre("[[:alpha:]]", "a", 1, "BRE: [:alpha:] match");
    bre("[[:alpha:]]", "1", 0, "BRE: [:alpha:] no match digit");
    bre("[[:digit:]]", "5", 1, "BRE: [:digit:] match");
    bre("[[:digit:]]", "a", 0, "BRE: [:digit:] no match alpha");
    bre("[[:space:]]", " ", 1, "BRE: [:space:] match space");
    bre("[[:space:]]", "\t", 1, "BRE: [:space:] match tab");
    bre("[[:space:]]", "a", 0, "BRE: [:space:] no match");
    bre("[[:upper:]]", "A", 1, "BRE: [:upper:] match");
    bre("[[:upper:]]", "a", 0, "BRE: [:upper:] no match lower");
    bre("[[:lower:]]", "a", 1, "BRE: [:lower:] match");
    bre("[[:alnum:]]", "a", 1, "BRE: [:alnum:] alpha");
    bre("[[:alnum:]]", "1", 1, "BRE: [:alnum:] digit");
    bre("[[:alnum:]]", " ", 0, "BRE: [:alnum:] space");
}

static void test_case_insensitive(void)
{
    printf("--- Case insensitive ---\n");
    bre_i("hello", "HELLO", 1, "BRE -i: upper");
    bre_i("hello", "Hello", 1, "BRE -i: mixed");
    bre_i("hello", "world", 0, "BRE -i: no match");
    bre_i("[a-z]+", "ABC", 1, "BRE -i: range");
}

static void test_complex(void)
{
    printf("--- Complex patterns ---\n");
    bre("[0-9][0-9]*\\.[0-9]", "3.14", 1, "BRE: float match");
    bre("[0-9][0-9]*\\.[0-9]", "3.1abc", 1, "BRE: float prefix match");
    bre("[0-9][0-9]*\\.[0-9]", "abc", 0, "BRE: float no match");
    bre("[0-9][0-9]*\\.[0-9]", "3.", 0, "BRE: float incomplete");
    ere("([0-9]+)\\.([0-9]+)", "3.14", 1, "ERE: float with groups");
    ere("[[:alpha:]]+[[:digit:]]+", "abc123", 1, "ERE: alpha then digit");
    ere("[[:alpha:]]+[[:digit:]]+", "123abc", 0, "ERE: digit before alpha fails");
    bre("^[[:space:]]*#", "# comment", 1, "BRE: comment line");
    bre("^[[:space:]]*#", "  # indented", 1, "BRE: indented comment");
    bre("^[[:space:]]*#", "not a comment", 0, "BRE: not a comment");
}

static void test_only_metachar(void)
{
    printf("--- Only metacharacters ---\n");
    bre(".*", "", 1, "BRE: .* matches empty");
    bre(".*", "anything", 1, "BRE: .* matches anything");
    bre("^$", "", 1, "BRE: ^$ matches empty");
    bre("^$", "x", 0, "BRE: ^$ fails nonempty");
    ere("[^a]", "b", 1, "ERE: [^a] matches non-a");
    ere("[^a]", "a", 0, "ERE: [^a] fails a");
}

static void test_backref_fallback(void)
{
    printf("--- Backref fallback (POSIX) ---\n");
    /* These patterns have backreferences and should fall back to POSIX.
     * We only verify they don't crash. */
    const char *err = NULL;
    mb_regex *re = mb_regex_compile("\\(a\\)\\1", SX_REG_BRE, &err);
    if (re) {
        /* Just run it and verify no crash */
        mb_regex_search(re, "aa", 2, NULL);
        mb_regex_search(re, "ab", 2, NULL);
        mb_regex_free(re);
        g_passed++;
        /* printf("  PASS: BRE backref \\1 doesn't crash\n"); */
    } else {
        /* Some engines reject backrefs entirely — that's OK since classifier
         * sends them to POSIX which handles the compile */
        g_passed++;
    }
}

static void test_empty_alternation(void)
{
    printf("--- Edge cases ---\n");
    ere("a|", "a", 1, "ERE: empty right alt matches a");
    ere("a|", "b", 1, "ERE: empty right alt matches empty");
    ere("|b", "b", 1, "ERE: empty left alt matches b");
    bre("^a\\{1,\\}", "aaa", 1, "BRE: \\{1,\\} matches multiple");
}

/*
 * Match bounds and the match-only fast path, both against libc.
 * The checks above only compare "matched or not" through the bounded
 * search, so they missed that the engine returned the SHORTEST match
 * (grep -o 'a.*' printed "a") and that the match-only path used by plain
 * grep never matched $ on a non-empty line.
 */
static void check_bounds(const char *pat, int mb_flags, int posix_flags,
                         const char *text, const char *label)
{
    const char *err = NULL;
    mb_regex *re = mb_regex_compile(pat, mb_flags | SX_REG_NOSUB, &err);
    regex_t pre;
    if (!re || regcomp(&pre, pat, posix_flags) != 0) {
        if (re) mb_regex_free(re);
        g_failed++;
        printf("  FAIL: %s (compile)\n", label);
        return;
    }
    size_t n = strlen(text);
    mb_match m = {NULL, NULL};
    int got   = mb_regex_search(re, text, n, &m);
    int fast  = mb_regex_search(re, text, n, NULL);
    regmatch_t pm;
    int want  = regexec(&pre, text, 1, &pm, 0) == 0;
    int ok = got == want && fast == want;
    if (ok && want)
        ok = (m.start - text) == pm.rm_so && (m.end - text) == pm.rm_eo;
    if (ok) {
        g_passed++;
    } else {
        g_failed++;
        printf("  FAIL: %s\n        pat='%s' text='%s' mb=%d fast=%d posix=%d",
               label, pat, text, got, fast, want);
        if (got)  printf(" mb=[%ld,%ld)", (long)(m.start - text), (long)(m.end - text));
        if (want) printf(" posix=[%ld,%ld)", (long)pm.rm_so, (long)pm.rm_eo);
        printf("\n");
    }
    regfree(&pre);
    mb_regex_free(re);
}

/* mb_regex_search_from: ^ must not match at the offset, only at s[0]. */
static void check_from(const char *pat, const char *text, size_t from,
                       long want_s, long want_e, const char *label)
{
    const char *err = NULL;
    mb_regex *re = mb_regex_compile(pat, SX_REG_NOSUB, &err);
    mb_match m = {NULL, NULL};
    int got = re && mb_regex_search_from(re, text, strlen(text), from, &m);
    long gs = got ? (long)(m.start - text) : -1;
    long ge = got ? (long)(m.end - text) : -1;
    if (gs == want_s && ge == want_e) {
        g_passed++;
    } else {
        g_failed++;
        printf("  FAIL: %s\n        pat='%s' text='%s' from=%zu got=[%ld,%ld) want=[%ld,%ld)\n",
               label, pat, text, from, gs, ge, want_s, want_e);
    }
    if (re) mb_regex_free(re);
}

static void test_bounds(void)
{
    printf("--- Match bounds (leftmost-longest) ---\n");
    check_bounds("a.*", 0, 0, "abc", "BRE: a.* takes the whole rest");
    check_bounds("b*c", 0, 0, "abbbc", "BRE: b*c from the first b");
    check_bounds("b*", 0, 0, "abbb", "BRE: b* is empty at 0");
    check_bounds("x*", 0, 0, "abc", "BRE: x* empty match at 0");
    check_bounds("a[a-z]*", 0, 0, "abcd ef", "BRE: class star is greedy");
    check_bounds("o\\{2\\}", 0, 0, "foo", "BRE: interval");
    check_bounds("a+", SX_REG_ERE, REG_EXTENDED, "baaa", "ERE: a+ greedy");
    check_bounds("(ab)+", SX_REG_ERE, REG_EXTENDED, "xababy", "ERE: group plus greedy");
    check_bounds("a|ab", SX_REG_ERE, REG_EXTENDED, "ab", "ERE: longest alternative");
    check_bounds("ab|a", SX_REG_ERE, REG_EXTENDED, "ab", "ERE: longest alternative, reversed");
    check_bounds("[0-9]+", SX_REG_ERE, REG_EXTENDED, "v12345x", "ERE: digits greedy");
    check_bounds("$", 0, 0, "abc", "BRE: $ alone on a non-empty line");
    check_bounds("x*$", 0, 0, "abc", "BRE: x*$ at the end");
    check_bounds("c$", 0, 0, "abc", "BRE: c$");
    check_bounds("a$", 0, 0, "xa", "BRE: a$ (libc fallback class)");
    check_bounds("a$", 0, 0, "ab", "BRE: a$ no match");
    check_bounds("^", 0, 0, "abc", "BRE: ^ alone");
    check_bounds("^a*", 0, 0, "aab", "BRE: ^a* greedy");
    check_bounds("\\(ab\\)\\1", 0, 0, "xababy", "BRE: backref bounds (libc fallback)");

    printf("--- Intervals (used to be treated as +) ---\n");
    check_bounds("ab\\{0,1\\}c", 0, 0, "ac", "BRE: \\{0,1\\} allows none");
    check_bounds("ab\\{0,1\\}c", 0, 0, "abbc", "BRE: \\{0,1\\} rejects two");
    check_bounds("x\\{2\\}", 0, 0, "axb", "BRE: \\{2\\} needs two");
    check_bounds("x\\{2,\\}", 0, 0, "axxxxb", "BRE: \\{2,\\} greedy");
    check_bounds("\\(ab\\)\\{2\\}", 0, 0, "xababy", "BRE: group interval");
    check_bounds("a{0,1}b", SX_REG_ERE, REG_EXTENDED, "b", "ERE: {0,1}");
    check_bounds("a{2,3}", SX_REG_ERE, REG_EXTENDED, "aaaa", "ERE: {2,3} greedy");
    check_bounds("(a|b){2}", SX_REG_ERE, REG_EXTENDED, "xaby", "ERE: group {2}");
    check_bounds("x{0}y", SX_REG_ERE, REG_EXTENDED, "xy", "ERE: {0}");

    printf("--- Anchors in the middle ---\n");
    check_bounds("a^b", 0, 0, "a^b", "BRE: ^ not first is literal");
    check_bounds("a$b", 0, 0, "a$b", "BRE: $ not last is literal");
    check_bounds("x*^", 0, 0, "c", "BRE: trailing ^ is literal");
    check_bounds("\\(^a\\)", 0, 0, "ab", "BRE: ^ after \\( is an anchor");
    check_bounds("a*^(b|)", SX_REG_ERE, REG_EXTENDED, "ab", "ERE: ^ after a* (was a*(^|b))");
    check_bounds("^^", SX_REG_ERE, REG_EXTENDED, "b", "ERE: ^^ is two anchors");
    check_bounds("$^", SX_REG_ERE, REG_EXTENDED, "", "ERE: $^ on an empty line");
    check_bounds("x$", SX_REG_ERE, REG_EXTENDED, "axbx", "ERE: x$ at the end");

    printf("--- Search from an offset ---\n");
    check_from("^a", "aaa", 1, -1, -1, "^a does not match mid-line");
    check_from("^a", "aaa", 0, 0, 1, "^a matches at 0");
    check_from("a", "aaa", 1, 1, 2, "plain a from offset 1");
    check_from("a$", "xaya", 2, 3, 4, "a$ from offset 2 (libc fallback)");
    check_from("^b\\|c", "abc", 1, 2, 3, "^ inside alternation not at offset");
}

/* ---- Main ----------------------------------------------------------------- */

int main(void)
{
    printf("=== test_regex ===\n");

    test_literals();
    test_anchors();
    test_dot();
    test_star();
    test_plus_quest();
    test_alternation();
    test_groups();
    test_charclasses();
    test_case_insensitive();
    test_complex();
    test_only_metachar();
    test_backref_fallback();
    test_empty_alternation();
    test_bounds();

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);

    if (g_failed > 0) {
        printf("FAIL: test_regex\n");
        return 1;
    }
    printf("PASS: test_regex\n");
    return 0;
}
