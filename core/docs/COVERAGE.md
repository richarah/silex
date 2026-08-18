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
| `module/registry.c` | 159 | Module loading needs `.so` files under `SILEX_MODULE_PATH`; no test builds one |
| `module/loader.c` | 98 | as above |
| `cache/hashmap.c` | 73 | Reached only through fscache, which is itself at 6.5% |
| `util/regex/charclass_re.c` | 97 | POSIX bracket expressions via the charclass module — same missing module infrastructure |
| `util/platform.c` | 16 | io_uring / inotify detection; the coverage run is rootless |
| `cache/fscache.c` | 139 (6.5%) | Basic stat caching only; TTL and mtime invalidation never triggered |

Worth fixing in that order. The module system is ~257 lines of entirely
untested code that ships in every binary, and building one throwaway `.so` in a
test would cover it along with `charclass_re.c`.

## How to regenerate

```sh
make coverage        # clean, instrumented rebuild, suites, summary
```

Uses `gcovr` when installed; otherwise falls back to `gcov -t` parsed per line.
The gcov used is the one matching `$(CC)` — reading gcc 15's notes with gcc
13's gcov prints `version 'B52*', prefer 'B33*'` and cannot be trusted.
