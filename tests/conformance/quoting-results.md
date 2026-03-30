# Quoting Conformance Results

**Date:** 2026-03-31
**matchbox version:** 0.2.0
**Reference shell:** dash (/usr/bin/dash)

## Summary

**54 passed, 0 failed, 0 skipped**

All Q-01 through Q-08 test cases pass. matchbox output matches dash exactly.

## Test Groups

| Group | Cases | Status |
|-------|-------|--------|
| Q-01 Nested quoting | 8 | PASS |
| Q-02 Word splitting with IFS | 4 | PASS |
| Q-03 Parameter expansion | 15 | PASS |
| Q-04 Command substitution | 8 | PASS |
| Q-05 Here-doc | 7 | PASS |
| Q-06 Case statement | 4 | PASS |
| Q-07 Subshell isolation | 3 | PASS |
| Q-08 eval | 5 | PASS |

## Bugs Fixed During Testing

1. **Q-05-4 / Q-08-2 (stdin EOF in pipeline last stage)**: When matchbox reads a
   script from stdin via `fgetc(stdin)`, the stdio `stdin` FILE* gets its EOF flag
   set after consuming the script. When a builtin runs in-process as the last stage
   of a pipeline (with stdin redirected via `dup2` to the pipe read end), the
   builtin reads nothing because `stdin` still has the EOF flag.
   Fix: `fflush(stdin); clearerr(stdin);` before `dup2()` in `exec_pipeline()`
   (`src/shell/exec.c`).

## Detailed Results

All 54 cases match dash output exactly. No known gaps.
