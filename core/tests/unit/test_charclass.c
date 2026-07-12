/* test_charclass.c — unit test for charclass LUT
 *
 * Verifies all 256 entries of char_class[] against expected ASCII
 * classification rules. Exits 0 on success, 1 on any failure.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

#include "util/charclass.h"

static int failures = 0;

/* CHECK(expr, "format", ...) — at least one format arg required by ISO C99 */
#define CHECK(expr, ...) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: "); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    int passed = 0;

    /* --- CC_ALPHA: [A-Za-z_] ------------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') ||
                       c == '_';
        int got = (char_class[c] & CC_ALPHA) != 0;
        CHECK(got == expected,
              "CC_ALPHA[0x%02x '%c']: expected %d got %d",
              c, (c >= 32 && c < 127) ? c : '.', expected, got);
    }
    printf("  CC_ALPHA: 53 entries\n");

    /* --- CC_ALNUM: [A-Za-z0-9_] ---------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') ||
                       c == '_';
        int got = (char_class[c] & CC_ALNUM) != 0;
        CHECK(got == expected,
              "CC_ALNUM[0x%02x '%c']: expected %d got %d",
              c, (c >= 32 && c < 127) ? c : '.', expected, got);
    }
    printf("  CC_ALNUM: 63 entries\n");

    /* --- CC_DIGIT: [0-9] ----------------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c >= '0' && c <= '9');
        int got = (char_class[c] & CC_DIGIT) != 0;
        CHECK(got == expected,
              "CC_DIGIT[0x%02x]: expected %d got %d", c, expected, got);
    }
    printf("  CC_DIGIT: 10 entries\n");

    /* --- CC_SPACE: [ \\t\\r\\f\\v] ------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c == ' ' || c == '\t' || c == '\r' ||
                        c == '\f' || c == '\v');
        int got = (char_class[c] & CC_SPACE) != 0;
        CHECK(got == expected,
              "CC_SPACE[0x%02x]: expected %d got %d", c, expected, got);
    }
    printf("  CC_SPACE: 5 entries\n");

    /* --- CC_NEWLINE: [\\n] ---------------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c == '\n');
        int got = (char_class[c] & CC_NEWLINE) != 0;
        CHECK(got == expected,
              "CC_NEWLINE[0x%02x]: expected %d got %d", c, expected, got);
    }
    printf("  CC_NEWLINE: 1 entry\n");

    /* --- CC_QUOTE: ['\"` ] ---------------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c == '\'' || c == '"' || c == '`');
        int got = (char_class[c] & CC_QUOTE) != 0;
        CHECK(got == expected,
              "CC_QUOTE[0x%02x '%c']: expected %d got %d",
              c, (c >= 32 && c < 127) ? c : '.', expected, got);
    }
    printf("  CC_QUOTE: 3 entries\n");

    /* --- CC_GLOB: [*?[\\]] ---------------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c == '*' || c == '?' || c == '[' || c == ']');
        int got = (char_class[c] & CC_GLOB) != 0;
        CHECK(got == expected,
              "CC_GLOB[0x%02x '%c']: expected %d got %d",
              c, (c >= 32 && c < 127) ? c : '.', expected, got);
    }
    printf("  CC_GLOB: 4 entries\n");

    /* --- CC_DELIM: [;|&()<>] ------------------------------------------- */
    for (int c = 0; c < 256; c++) {
        int expected = (c == ';' || c == '|' || c == '&' ||
                        c == '(' || c == ')' || c == '<' || c == '>');
        int got = (char_class[c] & CC_DELIM) != 0;
        CHECK(got == expected,
              "CC_DELIM[0x%02x '%c']: expected %d got %d",
              c, (c >= 32 && c < 127) ? c : '.', expected, got);
    }
    printf("  CC_DELIM: 7 entries\n");

    /* --- Inline helper consistency ------------------------------------- */
    for (int c = 0; c < 256; c++) {
        unsigned char uc = (unsigned char)c;
        CHECK(is_alpha_underscore(uc) == ((char_class[uc] & CC_ALPHA) != 0),
              "is_alpha_underscore mismatch at 0x%02x", c);
        CHECK(is_name_char(uc) == ((char_class[uc] & CC_ALNUM) != 0),
              "is_name_char mismatch at 0x%02x", c);
        CHECK(is_digit(uc) == ((char_class[uc] & CC_DIGIT) != 0),
              "is_digit mismatch at 0x%02x", c);
        CHECK(is_shell_space(uc) == ((char_class[uc] & CC_SPACE) != 0),
              "is_shell_space mismatch at 0x%02x", c);
        CHECK(is_shell_ws(uc) == ((char_class[uc] & (CC_SPACE | CC_NEWLINE)) != 0),
              "is_shell_ws mismatch at 0x%02x", c);
    }
    printf("  Inline helpers: 256*5 checks\n");

    /* --- Spot-check important chars ------------------------------------ */
    CHECK(is_alpha_underscore('_'), "_ is alpha_underscore");
    CHECK(is_alpha_underscore('A'), "A is alpha_underscore");
    CHECK(is_alpha_underscore('z'), "z is alpha_underscore");
    CHECK(!is_alpha_underscore('0'), "0 is not alpha_underscore");
    CHECK(!is_alpha_underscore(' '), "space is not alpha_underscore");
    CHECK(!is_alpha_underscore('\n'), "newline is not alpha_underscore");

    CHECK(is_name_char('_'), "_ is name_char");
    CHECK(is_name_char('9'), "9 is name_char");
    CHECK(!is_name_char('-'), "- is not name_char");
    CHECK(!is_name_char('.'), ". is not name_char");

    CHECK(is_shell_space(' '), "space is shell_space");
    CHECK(is_shell_space('\t'), "tab is shell_space");
    CHECK(!is_shell_space('\n'), "newline is not shell_space");
    CHECK(!is_shell_space('a'), "a is not shell_space");

    CHECK(is_shell_ws('\n'), "newline is shell_ws");
    CHECK(is_shell_ws('\t'), "tab is shell_ws");
    CHECK(!is_shell_ws('a'), "a is not shell_ws");

    passed = 256 * 8 + 256 * 5 + 19 - failures;
    (void)passed;

    if (failures == 0) {
        printf("test_charclass: all checks passed\n");
        return 0;
    } else {
        fprintf(stderr, "test_charclass: %d failure(s)\n", failures);
        return 1;
    }
}
