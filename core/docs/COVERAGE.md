# silex Code Coverage

**Measured:** 2026-08-18 (re-measured later the same day after the module
suites landed), version 0.3.0, `make coverage` (gcc 15 `--coverage -g -O0`,
summarised with `gcov-15`).

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

**10,201 of 18,404 executable lines (55.4%)**  (was 9,823 / 18,338 = 53.6%)

| Subsystem | Executable lines | Covered | Was (earlier on 2026-08-18) |
|-----------|------|----------|------|
| Shell | 7,010 | **75.6%** | 75.6% |
| Utilities | 575 | **64.5%** | 64.5% |
| Regex engine | 1,025 | **51.7%** | 45.9% (charclass_re.c 0 → 60.8%) |
| Module / cache / main | 580 | **48.2%** | 8.4% (registry.c 0 → 79.9%, loader.c 0 → 48.5%) |
| Core applets | 9,148 | **39.7%** | 39.7% |

## Shell subsystem

| File | Lines | Covered |
|------|-------|---------|
| `lexer.c` | 657 | **92.09%** |
| `parser.c` | 864 | **90.97%** |
| `expand.c` | 1745 | **79.43%** |
| `job.c` | 130 | **77.69%** |
| `redirect.c` | 191 | **70.68%** |
| `vars.c` | 317 | **68.77%** |
| `exec.c` | 2737 | **66.28%** |
| `shell.c` | 414 | **60.14%** |

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

**`util/regex/charclass_re.c` (97)** — the reason this file gave for its 0.0%,
"POSIX bracket expressions via the charclass module — same missing module
infrastructure", was wrong: the file implements bracket expressions
(`[[:alpha:]]`) and has nothing to do with the module system. The real cause
took two wrong answers to find, and both are worth recording.

*First wrong answer:* "it is covered by `tests/unit/test_regex.c`, which is a
separate uninstrumented binary, so this is a measurement gap." Half true —
`test_regex.c` does cover it (`BRE: [:alpha:] match` and twenty more), and the C
unit-test binaries are built with `CFLAGS_COMMON` and are indeed **not** part of
the instrumented build. But that cannot be the whole story, because
`tests/compat/run.sh` already had five `[[:alpha:]]`-style grep tests, and a
direct check confirms `silex grep '[[:alpha:]]'` does enter the file.

*The actual cause:* those five tests never ran silex's grep. `run_test` builds
the silex side as `eval "$SILEX $mb_cmd"`, which prefixes only the FIRST word,
so in

    printf 'abc123\n456\n' | grep '[[:alpha:]]'

the silex side was `silex printf … | /usr/bin/grep …` and the reference side was
`printf … | /usr/bin/grep …`. **The same GNU grep on both sides.** They could not
have failed for any grep bug and never entered silex's regex engine at all.
Rewritten on 2026-08-18 to read from a fixture file so `grep` is the first word,
and extended from 5 tests to 10 (negated classes, a class with a range, `-E`
with a repeat, `[[:punct:]]`, and the same through `sed`). All pass against GNU,
and the file went **0.00% -> 60.82%** on the next `make coverage` — which is the
proof that the pipeline, not the instrumentation, was the cause.

30 of this suite's 171 tests still contain a pipeline, so the same trap is live
in each of them — several are harmless (`find … | sort` is testing find, and the
host's sort is a fine way to order its output), but any test whose SUBJECT is a
later stage is measuring the host tool. Wrap those in `sh -c '…'`, as the xargs
tests already are, so every stage goes through silex. There is now a comment
saying so at the point where it bit.

The general point, since this file exists to be quoted: a 0.0% row means "the
instrumented binary never ran these lines". Before concluding *untested*, check
whether a C unit test or an external suite reaches the code — and before
concluding *measurement artefact*, check that the in-tree test you are crediting
actually runs the binary. `sed.c` at 29.7% while passing 50 GNU sed tests is the
milder, honest version of the first effect.

## How to regenerate

```sh
make coverage        # clean, instrumented rebuild, suites, summary
```

Uses `gcovr` when installed; otherwise falls back to `gcov -t` parsed per line.
The gcov used is the one matching `$(CC)` — reading gcc 15's notes with gcc
13's gcov prints `version 'B52*', prefer 'B33*'` and cannot be trusted.
