# silex Code Coverage

**Measured:** 2026-08-18, version 0.3.0, `make coverage` (gcc 15 `--coverage
-g -O0`, summarised with `gcov-15`).

Supersedes the 2026-03-30 / v0.2.0 figures, which were stale by five months and
roughly half the codebase: they were taken when `parser.c` was 591 lines (it is
now 864 executable) and before the sed rewrite, the regex engine work and most
of the POSIX conformance push.

## Read this before quoting a number

**The figure below covers the unit, compat and shell-conformance suites only.**
It does NOT include the external suites — Oils, smoosh, modernish, ShellSpec,
GNU coreutils, GNU sed, GNU grep — and those are what actually exercise the
applets. `sed.c` reads 29.7% here while passing 50 of GNU sed's own tests; take
the applet rows as "what the in-tree suites reach", not as "what is tested".

The old version of this file also credited a compat suite that had not run:
`make coverage` drove `tests/compat/run_compat.sh`, a file that does not exist,
with `|| true` swallowing the error. Fixed 2026-08-18 — the target now names the
scripts that are there, adds the shell conformance suite, and reports any suite
that fails to run instead of hiding it.

## Total

**9,823 of 18,338 executable lines (53.6%)**

| Subsystem | Executable lines | Covered |
|-----------|------|----------|
| Shell | 7,010 | **75.6%** |
| Utilities | 575 | **64.5%** |
| Regex engine | 1,025 | **45.9%** |
| Core applets | 9,148 | **39.7%** |
| Module / cache / main | 580 | **8.4%** |

## Shell subsystem

| File | Lines | Covered |
|------|-------|---------|
| `lexer.c` | 657 | **92.09%** |
| `parser.c` | 864 | **90.97%** |
| `expand.c` | 1715 | **83.32%** |
| `job.c` | 130 | **77.69%** |
| `redirect.c` | 191 | **70.68%** |
| `vars.c` | 321 | **69.16%** |
| `exec.c` | 2719 | **65.43%** |
| `shell.c` | 413 | **59.56%** |

The front of the pipeline is well covered; `exec.c` is both the largest file in
the tree and the least covered of the shell core, and its uncovered lines are
concentrated in job control and the rarer builtins.

## The genuine holes

These are 0% under every in-tree suite, and unlike the applets they have no
external suite covering for them:

| File | Lines | Why |
|------|-------|-----|
| `cache/hashmap.c` | 73 | Reached only through fscache, which is itself at 6.5% |
| `util/platform.c` | 16 | io_uring / inotify detection; the coverage run is rootless |
| `cache/fscache.c` | 139 (6.5%) | Basic stat caching only; TTL and mtime invalidation never triggered |

### Two entries removed from this list on 2026-08-18

**`module/loader.c` (98) and `module/registry.c` (159)** were the real thing —
~257 lines of untested code shipping in every binary — and are now covered by
`tests/unit/test_module.sh` (22 assertions) and a rewritten
`tests/security/test_module_security.sh` (18). Writing them turned up three
bugs, including one that meant at most a single module could load per process.

Note what the 0.0% sat next to: `make security-test` reported "module security
tests: 4 passed" the whole time. Every one of those assertions ran
`silex --load-module FILE` and checked for a non-zero exit — and there is no
`--load-module` flag, so silex rejected the unknown option without opening the
file. The suite was green regardless of its fixtures and would have stayed green
with `loader.c` deleted from the build. **A passing suite next to a 0% figure
means the suite is not reaching the code; believe the coverage number.**

**`util/regex/charclass_re.c` (97)** was never a hole, and the reason given for
it here — "via the charclass module — same missing module infrastructure" — was
wrong twice over: the file implements POSIX bracket expressions
(`[[:alpha:]]`), it has nothing to do with the module system, and
`tests/unit/test_regex.c` has covered it since it was written (`BRE: [:alpha:]
match` and twenty more). It read 0.0% because the C unit-test binaries are
built with `CFLAGS_COMMON` and are **not** part of the instrumented build, so
only what the instrumented `silex` executes is counted. That is a gap in the
measurement, not in the testing. `tests/compat/generate.sh` now also drives
bracket expressions through the `grep` and `sed` applets, so the figure reflects
the coverage that was already there.

The general point, since this file exists to be quoted: a 0.0% row means "the
instrumented binary never ran these lines during those three suites". It does
not by itself mean untested — check whether a C unit test or an external suite
reaches the code first. `sed.c` at 29.7% while passing 50 GNU sed tests is the
same effect in a milder form.

## How to regenerate

```sh
make coverage        # clean, instrumented rebuild, suites, summary
```

Uses `gcovr` when installed; otherwise falls back to `gcov -t` parsed per line.
The gcov used is the one matching `$(CC)` — reading gcc 15's notes with gcc
13's gcov prints `version 'B52*', prefer 'B33*'` and cannot be trusted.
