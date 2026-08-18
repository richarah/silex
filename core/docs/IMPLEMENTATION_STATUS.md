# GNU Flag Implementation Status

Last verified: 2026-08-18 (previous: 2026-03-31)

Reproduce every figure on this page with:

```sh
bash tests/conformance/new_flags_test.sh build/bin/silex
```

## Summary

- **Test infrastructure**: `tests/conformance/new_flags_test.sh`
- **Tests passing**: **28/28 (100%)** — was 22/30 (73%) on 2026-03-31
- **Critical path items**: 5/5 passing
- **Module dispatch**: wired into `grep`, `sort`, `xargs`, `cp` and `install`;
  `find` has the lookup but cannot yet integrate a module into its predicate
  tree (see below)

## What changed since the March snapshot

Every flag the March run listed as FAILING is implemented and tested:

| Flag | March | Now |
|------|-------|-----|
| `grep -h` (never print filename) | ✗ | ✓ |
| `grep -x` (match whole line) | ✗ | ✓ |
| `grep -b` (byte offset) | ✗ | ✓ |
| `grep -Z` (NUL-terminated filenames) | partial | ✓ |
| `install -D` (create leading dirs) | ✗ **CRITICAL** | ✓ |
| `sort -c` (check sorted) | ✗ | ✓ |
| `sort -R` (random sort) | ✗ | ✓ |
| `xargs -t` (trace commands) | ✗ | ✓ |

The March page also carried a "Priority Implementation Order" and a "Next
Steps" list, both of which are now entirely done; they have been removed rather
than left to read as outstanding work.

## Module dispatch: what the March page claimed, and what was true

The March page recorded "Module dispatch: ✓ Wired into all core tools". The
lookup call was indeed present in every tool, but in `cp` and `install` it was
reachable only from the SHORT-flag loop. An unrecognised long option fell into
that loop, was read as the short flag `-`, and was looked up under the
two-character name `"--"`. Since `modules/cp_reflink.c` advertises nothing but
long flags (`--reflink`, `--reflink=auto|always|never`), the module shipped in
this repository could not be reached by any invocation at all.

Separately, `module_load()` closed the descriptor it had dlopened through
`/proc/self/fd/N`, so every module reused the same path string and `dlopen`'s
pathname cache returned the FIRST module for all of them: at most one module
could load per process, chosen by `readdir` order.

Both are fixed as of 2026-08-18, and `cp --reflink=auto` now works end to end
against the shipped module. The general lesson is the one this page is an
instance of: a tick in a status document is not a test. The behaviour is now
pinned by `tests/unit/test_module.sh` and
`tests/security/test_module_security.sh`.

## Known limitation

`find` calls `registry_lookup()` for an unknown predicate but cannot act on a
hit: find evaluates a predicate TREE, and the module API's
`handler(argc, argv, flag_index)` has no way to inject a node into it. The call
site says so. Extending the module API for predicate injection is the fix, and
is not scheduled.

## Where the remaining gaps are recorded

Flags that are still absent, with the reason and an implementation estimate for
each, are in [FLAG_GAPS.md](FLAG_GAPS.md). That document is a triage of GNU's
full flag surface, not a to-do list — most entries are deliberately
unimplemented.
