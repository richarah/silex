# External Test Suite Scorecard

**Measured:** 2026-07-12, monorepo `core/`, glibc x86_64.

Before trusting any number here, read why the previous version of this file was
meaningless.

## Why the old numbers were meaningless

Every gate in the test infrastructure was wired so it could not report failure:

- **Six runners ended in a hardcoded `exit 0`.**
- **`run-configure.sh` tested `tail`'s exit status**, not configure's
  (`if cmd | tail -50; then`). `FAIL` could never increment, so the report
  always said `Failed: 0` — the exact string CI grepped for. A run in which
  curl's configure died with `'echo' command not found` was recorded as
  **5/5 PASS**.
- **`run-modernish.sh` counted `^FTL` lines with `grep -c … || true`.** When
  modernish failed to launch, the count was 0, so the suite reported
  *"Fatal bugs (FTL): 0 — Perfect score!"* — a clean sheet awarded **because**
  it was too broken to run.
- **`docker-run.sh`** — the only path CI exercised — turned every failure into
  `|| echo "… timed out or failed"` and had no `exit` at all.
- **CI's "critical requirements" step treated a missing results file as a
  pass**, printing `? results not found` and then `✓ All critical checks passed`.
- **The suites weren't running at all.** The Makefile passed a *relative* binary
  path to runners that immediately `cd` into their own repo directories, after
  which it stopped resolving. The `[ -x ]` guards sat before the `cd`, so they
  passed. Eight of ten suites executed **zero tests**. The old triage document
  blamed "WSL2 binary execution issues"; it was a harness bug that would have
  failed identically on native Linux and in CI.

Separately, the vendored smoosh checkout had been **corrupted by in-place test
runs**: 50 `.out` and ~180 `.err` expected-output files had been *generated* by
silex itself. Those tests compared silex against its own output and passed by
construction. Upstream tracks 147 `.out` files; 197 were on disk.

All of that is fixed. Suites can now fail, and a suite that executes zero tests
is a failure rather than a pass.

## Real numbers

| Suite | Result | Notes |
|-------|--------|-------|
| **Smoosh** | **125 / 186 (67%)** | Run via upstream's own `tests/shell_tests.sh` against a pristine checkout. This is the honest POSIX conformance figure. |
| Oils/OSH | not yet re-measured | Harness fixed; needs a clean run. |
| modernish | not yet re-measured | Blocked on a real silex bug — see below. |
| mksh | not yet re-measured | |
| GNU coreutils | not yet re-measured | |
| GNU grep | not yet re-measured | |
| GNU sed | not yet re-measured | |
| toybox | not yet re-measured | |
| ShellSpec | not yet re-measured | |
| Autoconf | not yet re-measured | Previously reported 5/5. The real figure was **2/5** — only sqlite and zlib. |

This repo previously contained **five mutually contradictory** smoosh figures
(0%, 51%, 59%, 62%, 66%). The 67% above is measured, reproducible, and fails the
build when it regresses:

```sh
make core-external-test
```

## Where the 61 smoosh failures are

Clustered, not scattered:

| Cluster | Approx. count |
|---|---|
| Background jobs / job control (`semantics.background.*`, `sh.monitor.*`, `builtin.jobs`, `builtin.kill.jobs`) | 8 |
| Traps in subshells (`builtin.trap.subshell.*`, `semantics.traps.*`, `semantics.return.trap`, `semantics.errexit.trap`) | 8 |
| Pattern matching (`semantics.pattern.bracket.quoted`, `.hyphen`, `.rightbracket`, `case.escape`) | 4 |
| `errexit` in subshells | 2 |
| Backtick fds / ppid | 2 |
| Escaping (backslash, single quote) | 2 |
| `exec`, `dot`, parse errors, exit codes | remainder |

## Real bugs this surfaced

1. **`V=${u:-$(echo A)}` fails in an assignment RHS** with `bad substitution`,
   while the identical expansion works fine as a command argument.
   `PREFIX=${PREFIX:-$(pwd)}` is a ubiquitous idiom, and this is what kills
   modernish (its bootstrap, line 142).
2. **`tail -N`** (the obsolescent form) is unsupported, while `head -N` works.

## Reproducing

```sh
make core-external-fetch            # one-time, ~500MB
make core-external-test             # all suites; fails if any suite fails
core/tests/external/run-smoosh.sh   # just smoosh
```

Suites run in a temp directory. If scratch files (`a1`, `cmd.sh`, `dir/`,
`link_*`) start appearing in the source tree, a runner has regressed to running
tests in place — which is how they came to be committed to this repo.
