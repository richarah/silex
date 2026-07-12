#ifndef SILEX_SECTION_H
#define SILEX_SECTION_H

/*
 * section.h — binary layout hints for GCC/Clang.
 *
 * HOT:  hot-path functions placed in .text.hot (linked before .text).
 * WARM: moderately-frequent functions.
 * COLD: error paths, rarely-used flags, init/teardown.
 *
 * The linker script (or GNU ld default) places .text.hot early in the
 * text segment, improving I-cache locality for frequently executed code.
 *
 * Usage: HOT static int my_func(void) { ... }
 */

#if defined(__GNUC__) || defined(__clang__)
#  define HOT  __attribute__((section(".text.hot")))
#  define WARM __attribute__((section(".text.warm")))
#  define COLD __attribute__((section(".text.cold"), cold))
#  define likely(x)   __builtin_expect(!!(x), 1)
#  define unlikely(x) __builtin_expect(!!(x), 0)
#else
#  define HOT
#  define WARM
#  define COLD
#  define likely(x)   (x)
#  define unlikely(x) (x)
#endif

#endif /* SILEX_SECTION_H */
