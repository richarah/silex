# GNU Flag Implementation Status

Generated: 2026-03-31

## Summary

- **Test infrastructure**: ✓ Created (`tests/conformance/new_flags_test.sh`)
- **Module dispatch**: ✓ Wired into all core tools
- **Tests passing**: 22/30 (73%)
- **Critical path items**: 4/5 passing

## Test Results

### PASSING (22 tests)

**grep (8/12):**
- ✓ -A N (after-context)
- ✓ -B N (before-context)
- ✓ -C N (context)
- ✓ -o (only-matching)
- ✓ -m N (max-count)
- ✓ -L (files-without-match)
- ✓ -H (always print filename)
- ✓ -Z (NUL-terminated filenames) - partial

**find (3/3):**
- ✓ -print0 (NUL-terminated) **CRITICAL**
- ✓ -newer FILE (modification time)
- ✓ -size N[cwbkMG]

**xargs (4/5):**
- ✓ -L N (max-lines) **CRITICAL**
- ✓ -a FILE (read from file)
- ✓ -s N (max command line)
- ✓ (general functionality)

**sed (1/1):**
- ✓ -f FILE (script from file) **CRITICAL**

**sort (1/3):**
- ✓ -M (month sort)

**install (2/4):**
- ✓ -v (verbose)
- ✓ -t DIR (target directory)

### FAILING (8 tests)

**grep (4 flags):**
- ✗ -h (never print filename) - ~10 lines
- ✗ -x (match whole line) - ~15 lines
- ✗ -b (byte offset) - ~20 lines
- ✗ -Z (NUL-terminated) - needs fix ~5 lines

**install (2 flags):**
- ✗ -D (create leading dirs) **CRITICAL** - ~80 lines
- ✗ (exit code issue with -D)

**sort (2 flags):**
- ✗ -c (check sorted) - ~40 lines
- ✗ -R (random sort) - ~30 lines

**xargs (1 flag):**
- ✗ -t (trace commands) - ~15 lines

**Total new code needed: ~215 lines** (much less than the ~5800 estimated!)

## Priority Implementation Order

### P0 - Critical Path (1 item, ~80 lines)
1. **install -D** - Makefiles depend on this

### P1 - High Usage (4 items, ~60 lines)
2. grep -h (never print filename)
3. grep -x (match whole line)
4. grep -b (byte offset)
5. xargs -t (trace)

### P2 - Nice to Have (2 items, ~70 lines)
6. sort -c (check sorted)
7. sort -R (random sort)

### P3 - Polish (1 item, ~5 lines)
8. grep -Z fix (NUL-terminated filenames)

## Next Steps

1. ✓ Create test infrastructure
2. → Implement install -D (~80 lines)
3. → Implement 4 grep flags (~50 lines)
4. → Implement xargs -t (~15 lines)
5. → Implement 2 sort flags (~70 lines)
6. → Fix grep -Z edge case (~5 lines)
7. Run full test suite
8. Update FLAG_GAPS.md with actual status
