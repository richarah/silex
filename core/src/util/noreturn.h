#ifndef SILEX_NORETURN_H
#define SILEX_NORETURN_H

/* SILEX_NORETURN — "this function never comes back".
 *
 * C11 spells it `_Noreturn`, and that is what the compiler wants. It is not
 * what the ANALYSERS want: cppcheck 2.21 ignores `_Noreturn` outright and
 * reports every line after such a call as reachable. That is not a cosmetic
 * complaint -- it turns each of silex's fatal-error paths into a false "division
 * by zero" or "use after free", and a static-analysis gate whose output is
 * mostly false is a gate nobody reads.
 *
 * GCC and Clang both understand `__attribute__((noreturn))`, and so does
 * cppcheck, so prefer it and fall back to the standard spelling elsewhere.
 * Place it BEFORE the return type on both the declaration and the definition.
 *
 * cppcheck predefines neither `__GNUC__` nor `__clang__`, and its own
 * `__cppcheck__` is not visible to `#if defined(...)` in a header, so it would
 * take the `_Noreturn` branch -- the one it ignores -- and the whole exercise
 * would achieve nothing. The `cppcheck` make target passes `-D__GNUC__` for
 * exactly this reason: it IS analysing a GCC build. */
#if defined(__GNUC__) || defined(__clang__)
#  define SILEX_NORETURN __attribute__((noreturn))
#else
#  define SILEX_NORETURN _Noreturn
#endif

#endif /* SILEX_NORETURN_H */
