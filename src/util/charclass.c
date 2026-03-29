/* charclass.c — ASCII character classification table definition
 *
 * Entries marked with a combination of CC_* flags from charclass.h.
 * Unset bytes default to 0 (no classification).
 * Uses C99/C11 designated initialisers; non-designated entries are zero.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "charclass.h"

/* Shorthand for entries that belong to multiple classes */
#define _AN  (CC_ALPHA | CC_ALNUM)         /* [A-Za-z_]    */
#define _ND  (CC_ALNUM | CC_DIGIT)         /* [0-9]        */

const uint8_t char_class[256] = {
    /* Control characters: 0x00–0x08 — all zero */

    /* 0x09 \t  — horizontal tab: non-newline space */
    ['\t'] = CC_SPACE,
    /* 0x0a \n  — newline */
    ['\n'] = CC_NEWLINE,
    /* 0x0b \v  — vertical tab */
    ['\v'] = CC_SPACE,
    /* 0x0c \f  — form feed */
    ['\f'] = CC_SPACE,
    /* 0x0d \r  — carriage return */
    ['\r'] = CC_SPACE,
    /* 0x0e–0x1f — all zero */

    /* 0x20 ' ' — space */
    [' ']  = CC_SPACE,

    /* Shell metacharacters */
    ['"']  = CC_QUOTE,   /* 0x22 */
    ['&']  = CC_DELIM,   /* 0x26 */
    ['\''] = CC_QUOTE,   /* 0x27 */
    ['(']  = CC_DELIM,   /* 0x28 */
    [')']  = CC_DELIM,   /* 0x29 */
    ['*']  = CC_GLOB,    /* 0x2a */

    /* Digits 0x30–0x39 */
    ['0']  = _ND,
    ['1']  = _ND,
    ['2']  = _ND,
    ['3']  = _ND,
    ['4']  = _ND,
    ['5']  = _ND,
    ['6']  = _ND,
    ['7']  = _ND,
    ['8']  = _ND,
    ['9']  = _ND,

    ['<']  = CC_DELIM,   /* 0x3c */
    ['>']  = CC_DELIM,   /* 0x3e */

    /* Uppercase A–Z 0x41–0x5a */
    ['A']  = _AN, ['B']  = _AN, ['C']  = _AN, ['D']  = _AN,
    ['E']  = _AN, ['F']  = _AN, ['G']  = _AN, ['H']  = _AN,
    ['I']  = _AN, ['J']  = _AN, ['K']  = _AN, ['L']  = _AN,
    ['M']  = _AN, ['N']  = _AN, ['O']  = _AN, ['P']  = _AN,
    ['Q']  = _AN, ['R']  = _AN, ['S']  = _AN, ['T']  = _AN,
    ['U']  = _AN, ['V']  = _AN, ['W']  = _AN, ['X']  = _AN,
    ['Y']  = _AN, ['Z']  = _AN,

    ['[']  = CC_GLOB,    /* 0x5b */
    [']']  = CC_GLOB,    /* 0x5d */

    /* 0x5f '_' — underscore: identifier start and continuation */
    ['_']  = _AN,

    /* Backtick 0x60 */
    ['`']  = CC_QUOTE,

    /* Lowercase a–z 0x61–0x7a */
    ['a']  = _AN, ['b']  = _AN, ['c']  = _AN, ['d']  = _AN,
    ['e']  = _AN, ['f']  = _AN, ['g']  = _AN, ['h']  = _AN,
    ['i']  = _AN, ['j']  = _AN, ['k']  = _AN, ['l']  = _AN,
    ['m']  = _AN, ['n']  = _AN, ['o']  = _AN, ['p']  = _AN,
    ['q']  = _AN, ['r']  = _AN, ['s']  = _AN, ['t']  = _AN,
    ['u']  = _AN, ['v']  = _AN, ['w']  = _AN, ['x']  = _AN,
    ['y']  = _AN, ['z']  = _AN,

    ['|']  = CC_DELIM,   /* 0x7c */

    /* 0x3b ';' */
    [';']  = CC_DELIM,

    /* 0x3f '?' */
    ['?']  = CC_GLOB,
};

#undef _AN
#undef _ND
