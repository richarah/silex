/* charclass.h — ASCII character classification lookup table
 *
 * Provides a single 256-byte table indexed by (unsigned char) so that
 * identifier scanning, whitespace detection, and similar hot-path checks
 * reduce to a single array load + AND instead of a chain of comparisons.
 *
 * All helpers take `unsigned char` to avoid signed-extension bugs; callers
 * must cast: is_name_char((unsigned char)c).
 */
#ifndef SILEX_CHARCLASS_H
#define SILEX_CHARCLASS_H

#include <stdint.h>

/* Bit flags ---------------------------------------------------------------- */
#define CC_ALPHA    ((uint8_t)0x01u)  /* [A-Za-z_]     — identifier start    */
#define CC_ALNUM    ((uint8_t)0x02u)  /* [A-Za-z0-9_]  — identifier continue */
#define CC_DIGIT    ((uint8_t)0x04u)  /* [0-9]                                */
#define CC_SPACE    ((uint8_t)0x08u)  /* [ \t\r\f\v]   — non-newline space   */
#define CC_NEWLINE  ((uint8_t)0x10u)  /* [\n]                                 */
#define CC_QUOTE    ((uint8_t)0x20u)  /* ['"` ]                               */
#define CC_GLOB     ((uint8_t)0x40u)  /* [*?[\]]                              */
#define CC_DELIM    ((uint8_t)0x80u)  /* [;|&()<>]     — shell meta           */

extern const uint8_t char_class[256];

/* Inline helpers ----------------------------------------------------------- */

/* True for [A-Za-z_] — valid identifier first character */
static inline int is_alpha_underscore(unsigned char c)
{
    return (char_class[c] & CC_ALPHA) != 0;
}

/* True for [A-Za-z0-9_] — valid identifier continuation character */
static inline int is_name_char(unsigned char c)
{
    return (char_class[c] & CC_ALNUM) != 0;
}

/* True for [0-9] */
static inline int is_digit(unsigned char c)
{
    return (char_class[c] & CC_DIGIT) != 0;
}

/* True for [ \t\r\f\v] — non-newline whitespace (ASCII only) */
static inline int is_shell_space(unsigned char c)
{
    return (char_class[c] & CC_SPACE) != 0;
}

/* True for any POSIX whitespace including \n */
static inline int is_shell_ws(unsigned char c)
{
    return (char_class[c] & (CC_SPACE | CC_NEWLINE)) != 0;
}

#endif /* SILEX_CHARCLASS_H */
