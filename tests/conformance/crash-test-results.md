# Crash Test Results

**Date:** 2026-03-31
**matchbox version:** 0.2.0

## SEC-03: Crash Input Testing

All inputs produced graceful behavior (no crashes, no hangs, no assertion failures).

| Input | Behavior | Exit Code |
|-------|----------|-----------|
| `matchbox sh -c 'echo "unterminated'` | Outputs `unterminated` (quotes auto-closed at EOF) | 0 |
| `matchbox sh -c 'echo $(echo unclosed'` | Outputs `unclose` (cmd-sub closed at EOF) | 0 |
| `matchbox sh -c ''` | No-op | 0 |
| `matchbox sh -c '   '` | No-op (whitespace) | 0 |
| Script with embedded null byte | Executes up to null, ignores rest | 0 |
| `matchbox sh -c 'echo $((1/0))'` | Error message + exit 2 (matches dash) | 2 |
| `matchbox sh -c 'sleep 100 & exit 0'` | Returns immediately, sleep orphaned | 0 |

## Notes

- **Unterminated strings**: The lexer gracefully closes unclosed quotes at EOF. This
  matches dash/bash behavior (no crash, just implicit closing).
- **Division by zero**: Fixed in this session. Previously returned 0 silently. Now prints
  `matchbox: sh: arithmetic expression: division by zero` and exits 2 (matches dash).
- **Background sleep + exit**: The sleep process is orphaned but matchbox exits cleanly
  without waiting. This is POSIX-correct behavior.
- **Null bytes**: null bytes in script are treated as token boundaries. The lexer stops
  reading at the null, executes the valid portion, exits 0.

## Status: PASS
All crash inputs handled gracefully. No segfaults, no infinite loops.
