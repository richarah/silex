# GNU Comparison Results

**Date:** 2026-03-31
**matchbox version:** 0.2.0
**Reference:** System GNU tools

## Summary

**42 passed, 0 failed**

All builtin output and exit codes match GNU equivalents.

## Builtins Tested

| Builtin | Cases | Status |
|---------|-------|--------|
| sort | 4 | PASS |
| grep | 8 | PASS |
| wc | 3 | PASS |
| sed | 4 | PASS |
| head | 2 | PASS |
| tail | 2 | PASS |
| cut | 3 | PASS |
| tr | 3 | PASS |
| basename | 2 | PASS |
| dirname | 2 | PASS |
| find | 3 | PASS |
| cat | 2 | PASS |
| cp (exit codes) | 1 | PASS |
| mkdir (exit codes) | 1 | PASS |
| env | 1 | PASS |
| stat (exit codes) | 1 | PASS |

## Notes

All builtins match GNU tool output and exit codes for the tested argument combinations.
Sort, grep, sed, wc, head, tail, cut, tr, basename, dirname, find, and cat produce
byte-identical output to the system GNU tools for standard operations.
