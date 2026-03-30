# matchbox — Status Audit

**Date:** 2026-03-30 (v0.2.0 released; post-release polish and v0.3.0 prep)
**Binary:** `build/bin/matchbox` — ~1.6 MB musl static (release), ~318 KB glibc dynamic PIE
**Version:** 0.2.0 (released 2026-03-30)

---

## 1. File Inventory

| Subsystem | Source files (.c) |
|-----------|-------------------|
| Core builtins | 27 (`src/core/`) |
| Shell | 8 (`src/shell/`) |
| Utilities | 14 (`src/util/`) |
| Regex engine | 6 (`src/util/regex/`) |
| Module system | 2 (`src/module/`) |
| Batch I/O | 3 (`src/batch/`) |
| Cache | 2 (`src/cache/`) |
| Main | 1 (`src/main.c`) |
| **Total** | **63** |

Headers: matching `.h` for every `.c` plus `matchbox_module.h` (public module API).

Test directories: `tests/{unit,integration,compat,security,stress,edge,bench,fuzz}`.

---

## 2. Builtin Completeness

### External tool builtins (32)

basename, cat, chmod, cp, cut, date, dirname, echo, env, find, grep, head, install, ln, mkdir, mktemp, mv, printf, readlink, realpath, rm, sed, sha256sum, sort, stat, tail, tee, touch, tr, wc, xargs, sh

All 32 are implemented and present (5 new in v0.2.0: env, mktemp, realpath, sha256sum, tee).

### Shell builtins (in shell/exec.c)

cd, pwd, exit, export, unset, set, shift, source/., :, true, false, exec, eval, read, wait, kill, jobs, bg, fg, umask, command, type, getopts, break, continue, return, local, readonly, trap

---

## 3. Shell Conformance

Tested with `tests/unit/shell/*.sh` against `build/bin/matchbox`.

| Suite | Pass | Fail | Notes |
|-------|------|------|-------|
| test_control.sh | **40** | **0** | All fixed |
| test_expansion.sh | **47** | **0** | Fixed: bitshift `<<`/`>>`, compound assign `+=` etc., bitwise ops |
| test_functions.sh | **25** | **0** | All fixed |
| test_pipes.sh | **17** | **0** | Fixed: `head -1` legacy shorthand, waitpid EINTR |
| test_redirects.sh | **22** | **1** | `&>` bash extension (SKIP annotated, not POSIX) |
| test_variables.sh | **34** | **0** | Fixed test bug; added F-03 IFS edge cases (5 tests) |
| test_errexit.sh | **10** | **0** | New: `set -e` edge cases (Phase F-01) |
| test_trap.sh | **8** | **0** | New: trap edge cases (Phase F-02) |
| **Total** | **203** | **1** | 182→203 (+21 new tests) |

Fixed in v0.2.0:
- `$((x << n))` / `$((x >> n))` bitshift: implemented
- `$((x += n))` and all compound assignment operators: implemented
- Bitwise operators `&`, `|`, `^`, `~`: implemented
- `head -1` legacy shorthand: implemented
- `waitpid` EINTR retry: fixed (signals during waitpid no longer corrupt status)
- Keywords in argument position (e.g. `echo done`, `echo fi`): fixed — POSIX-correct

Remaining gap:
- `&>` combined stdout+stderr redirect: bash extension, not POSIX (SKIP annotated)

---

## 4. Module System

- API version: `MATCHBOX_MODULE_API_VERSION = 2`
- New in v2: `libc` field (`"musl"` or `"glibc"`), `MATCHBOX_EXPORT` macro, `MATCHBOX_LIBC_NAME`
- Module directories: `/usr/lib/matchbox/modules` (glibc), `/usr/lib/matchbox/modules-musl` (musl)
- Registry: 64 buckets, max 1 024 entries, invalidates on directory mtime change
- Security checks on load: no symlinks, no world-writable, owner must be uid 0 or self, libc tag match, api_version match

---

## 5. Optimisations Implemented

| ID | Description | Status |
|----|-------------|--------|
| O-13 | Thompson NFA/DFA regex replacing POSIX `regexec` | DONE |
| O-14 | 128 KB stdout buffer when `!isatty` | DONE |
| O-15 | FNV-1a string intern table | DONE |
| O-16 | `mmap` input for `sort` on files > 64 KB | DONE |
| O-17 | WC lines-only fast path | DONE |
| O-18 | Per-file HOT (`-O3`) / COLD (`-Os`) compilation | DONE |
| O-19 | PATH lookup cache in `shell_ctx_t` (256 slots) | DONE |
| F-03 | `likely()`/`unlikely()` hints in arena, exec, fork | DONE |
| F-05 | PATH FNV-1a cache with `execv` instead of `execvp` | DONE |
| L-02 | `cat \| applet` pipe elimination (trivial cat, no fork/pipe) | DONE |

Regex classifier dispatch order: BMH (literal) → prefix-anchor → full Thompson NFA.

---

## 5b. Memory Safety Structural Audit

Audit performed 2026-03-30. All FAIL/MISSING items were fixed.

| Component | Check | Before | After |
|-----------|-------|--------|-------|
| Arena allocator | Max size cap | MISSING | FIXED: `ARENA_MAX_BYTES = 64 MB` in arena.h; abort on excess |
| Arena allocator | `total_bytes` tracking | MISSING | FIXED: field in `arena_t`; incremented in `arena_alloc` |
| Arena allocator | Reset between shell commands | FAIL (causes UAF with lexer lookahead) | MITIGATED: 64 MB cap prevents unbounded growth; reset not safe |
| `strbuf_t` | Max capacity | MISSING | FIXED: `SB_MAX_CAP = 64 MB`; `sb_grow` returns -1 on excess |
| Intern table | Entry count cap | MISSING | FIXED: `INTERN_MAX_ENTRIES = 1 000 000`; returns original ptr at cap |
| Intern table | String length bound | MISSING | FIXED: rejects `n > PATH_MAX`; returns original ptr |
| `fscache` | Entry count cap | MISSING | FIXED: `FSCACHE_MAX_ENTRIES = 100 000`; skips caching at cap |
| Module registry | Entry count cap | MISSING | FIXED: `REGISTRY_MAX_ENTRIES = 1 024`; `registry_count` tracking |
| Thompson `addstate()` | Recursion depth guard | FAIL (depth unbounded up to `MB_MAX_INSTRS`) | FIXED: `depth` parameter; returns at `MB_MAX_INSTRS` |
| DFA cache | Slot count cap | PASS | `DFA_CACHE_MAX = 256` (pre-existing) |
| NFA bytecode | Instruction count cap | PASS | `MB_MAX_INSTRS = 4096` (pre-existing) |
| io_uring queue | Depth cap | PASS | 256 (pre-existing) |
| Stack usage | VLA with user-controlled size | PASS | None present |
| Stack usage | Recursion cap | PASS | `SHELL_MAX_CALL_DEPTH = 1000` in exec.c |
| Stack usage | `alloca()` | PASS | None present |

**Note:** Makefile now uses `-MMD -MP` to generate header dependency files. This prevents stale object files when headers change (was the root cause of the stack-smashing regression found and fixed this session).

---

## 6. Performance Targets

Measured on host (glibc dynamic build, x86-64-v3, no musl).

| Metric | matchbox | Reference | Ratio |
|--------|----------|-----------|-------|
| Startup latency (`sh -c 'exit 0'`, 100 iters) | ~690 µs | ~670 µs (dash) | 1.03× |
| `grep` 10 000-line file (100 iters) | 1.48 ms/iter | 0.98 ms/iter (GNU grep) | 1.51× slower |
| `sort` 10 000-line file (100 iters) | 3.28 ms/iter | 5.41 ms/iter (GNU sort) | 1.65× faster |
| Binary size (glibc) | 292 KB | — | — |

Startup is within 3% of dash. grep is ~50% slower than GNU grep (expected: GNU grep has PCRE-JIT and AVX2 SIMD for character scanning). Sort is 65% faster than GNU sort on this workload.

---

## 7. Benchmark Comparison

See Section 6. Full benchmark scripts in `tests/bench/` with resource caps:

- `-v 4194304` (4 GB VM per process)
- `-f 1048576` (1 GB max file size)
- `-t 300` (5 min CPU)
- `-d 2097152` (2 GB data segment)

PGO workload suite (`pgo/`): **NOT PRESENT** — directory does not exist; `make pgo` not available.

---

## 8. Build Configurations

| Target | CFLAGS | LDFLAGS | Libc | Notes |
|--------|--------|---------|------|-------|
| `make` / `make all` | CFLAGS_RELEASE | LDFLAGS_GLIBC | glibc | Default; auto-detects musl-gcc |
| `make release` | CFLAGS_RELEASE -DMATCHBOX_LIBC_MUSL | LDFLAGS_MUSL (-static-pie) | musl | Requires musl-gcc |
| `make release-glibc` | CFLAGS_RELEASE -DMATCHBOX_LIBC_GLIBC | LDFLAGS_GLIBC (-pie) | glibc | Dynamic PIE |
| `make release-docker` | — | — | musl | Builds via Alpine container |
| `make debug` | CFLAGS_DEBUG (-O0 -g3 -fsanitize=address,undefined) | — | glibc | No LTO, no hardening |

No musl-gcc on this machine. `make` builds glibc dynamic PIE.

---

## 9. Security Status

| Check | Status |
|-------|--------|
| `BIND_NOW` (full RELRO) | PASS — present in dynamic section FLAGS |
| `GNU_RELRO` segment | PASS — present in program headers |
| `GNU_STACK` non-executable | PASS — RW (no X bit) |
| `-fvisibility=hidden` | PASS — applied in CFLAGS_RELEASE |
| `-fstack-protector-strong` | PASS — applied in all builds |
| `-fno-unwind-tables` | PASS — removes 10-15 KB of unwind data |
| `--gc-sections` | PASS — dead code elimination at link time |
| `--build-id=sha1` | PASS — content-based build ID |
| No setuid / setgid | PASS — matchbox does not call setuid/setgid |
| Module: no symlink load | PASS — lstat + S_ISLNK check |
| Module: no world-writable | PASS — `lst.st_mode & S_IWOTH` check |
| Module: owner check | PASS — uid 0 or self |
| Module: libc tag match | PASS — glibc/musl mismatch rejected |

---

## 10. Test Suite Status

| Suite | Command | Pass | Fail | Total |
|-------|---------|------|------|-------|
| Unit (sh scripts) | `make test` | 4 | 0 | 4 suites |
| echo | — | 27 | 0 | 27 |
| mkdir | — | 17 | 0 | 17 |
| cp | — | 25 | 0 | 25 |
| shell_builtins | — | 19 | 0 | 19 |
| test_charclass (C) | — | PASS | 0 | all |
| test_linescan (C) | — | PASS | 0 | all |
| test_regex (C) | — | 111 | 0 | 111 |
| Compat (TAP) | `make compat-test` | **167** | **0** | **167** |
| Integration | `make integration-test` | 36 | 0 | 36 |
| Security (rm safety) | `make security-test` | 22 | 0 | 22 |
| Shell: test_control | `tests/unit/shell/test_control.sh` | **40** | **0** | 40 |
| Shell: test_functions | `tests/unit/shell/test_functions.sh` | **25** | **0** | 25 |
| Shell: test_redirects | `tests/unit/shell/test_redirects.sh` | **22** | **1** | 23 |
| Shell: test_variables | `tests/unit/shell/test_variables.sh` | **34** | **0** | 34 |
| Shell: test_expansion | `tests/unit/shell/test_expansion.sh` | **47** | **0** | 47 |
| Shell: test_pipes | `tests/unit/shell/test_pipes.sh` | **17** | **0** | 17 |
| Shell: test_errexit | `tests/unit/shell/test_errexit.sh` | **10** | **0** | 10 |
| Shell: test_trap | `tests/unit/shell/test_trap.sh` | **8** | **0** | 8 |
| **Shell total** | — | **203** | **1** | **204** |

**Compat: 167/167 (all passing)** (up from 54 in v0.2.0; 113 new tests added for previously-uncovered builtins)

**Shell conformance failures (1):**
1. `test_redirects`: `&>` bash extension (not POSIX) — annotated as SKIP

---

## 11. Deferred / Outstanding Work

| Item | Status | Notes |
|------|--------|-------|
| PGO workload suite (`pgo/`) | MISSING | Directory does not exist |
| `make pgo` target | NOT FUNCTIONAL | Depends on absent pgo/ submodule |
| Shell: `&>` redirect | KNOWN GAP | bash extension, not targeted |
| v0.2.0 Phases A-G | **COMPLETE** | All planned v0.2.0 features shipped |
| v0.2.0 Phase H | **COMPLETE** | MATCHBOX_FORCE_FALLBACKS + aarch64 CI job |
| v0.2.0 Phase I | **COMPLETE** | All docs updated; COVERAGE.md generated |
| v0.2.0 Phase J | **COMPLETE** | check.sh exits 0 (10/10 PASS) |
| Post-v0.2.0 COV | **COMPLETE** | Compat tests expanded 54→167 (113 new); grep -H/-L, readlink -f, date -u %Z, realpath nonexistent all fixed |
| Post-v0.2.0 FP | **COMPLETE** | sha256sum error format; header comments; MARCH→x86-64-v2; build reproducibility verified |
| Post-v0.2.0 L-01 | **COMPLETE** | `docs/TAR_FEASIBILITY.md` written |
| Post-v0.2.0 L-02 | **COMPLETE** | `cat \| applet` pipe elimination in exec_pipeline; `docs/PIPE_ELIMINATION.md` |
| Thread-based pipeline | v0.3.0 | Design in PIPE_ELIMINATION.md |
| tar builtin | v0.3.0 | Design in TAR_FEASIBILITY.md |

---

## 12. Documentation

| File | Status | Contents |
|------|--------|---------|
| `README.md` | Present | Overview, building, requirements, targets |
| `SECURITY.md` | Present | Resource caps, signal handling, privilege, env vars, reproducibility |
| `ARCHITECTURE.md` | Present | Code conventions, naming, file headers |
| `COMPATIBILITY.md` | Present | Exit status table, locale note, POSIX conventions |
| `OPTIMISATIONS.md` | Present | Per-optimisation entries, performance gap analysis |
| `COVERAGE.md` | Present | Per-component gcov line coverage (v0.2.0) |
| `docs/UNIKERNEL.md` | Present | Feasibility study (Unikraft viable) |
| `docs/TAR_FEASIBILITY.md` | Present | tar builtin feasibility study (v0.3.0 plan) |
| `docs/PIPE_ELIMINATION.md` | Present | Builtin-to-builtin pipe elimination design |
| `PROMPT.md` | Present | Original specification |

---

## 13. CI Status

11 jobs defined in `.github/workflows/ci.yml`:

| Job | Environment |
|-----|-------------|
| `build-test-gcc` | ubuntu-latest, gcc |
| `asan` | ubuntu-latest, ASan/UBSan |
| `compat` | ubuntu-latest, TAP compat tests |
| `static-analysis` | ubuntu-latest, cppcheck |
| `shellcheck` | ubuntu-latest |
| `binary-size` | alpine:latest (`make release`) |
| `aarch64` | ubuntu-latest, qemu-aarch64 cross |
| `test-musl` | alpine:latest |
| `test-glibc` | ubuntu-latest |
| `test-musl-asan` | alpine:latest, debug |
| `test-glibc-asan` | ubuntu-latest, debug |

---

## 14. PGO Workload Suite

**Status: NOT PRESENT.**

- `pgo/` directory: does not exist
- `workloads/` directory: does not exist
- `make pgo`: target exists in Makefile but depends on `pgo/` submodule
- No PGO build has been performed

To add PGO: create `pgo/` with representative container build workloads, then:
```
make pgo     # instrument build + run workloads + rebuild with profile data
```

---

## 15. Compiler Flags

**CFLAGS_RELEASE (current default):**
```
-std=c11 -Wall -Wextra -Werror -pedantic
-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
-DMATCHBOX_VERSION="0.2.0" -DMATCHBOX_LIBC_GLIBC=1
-fstack-protector-strong
-O2 -flto=auto -fPIE
-march=x86-64-v3
-ffunction-sections -fdata-sections -fmerge-all-constants
-fno-unwind-tables -fno-asynchronous-unwind-tables
-fvisibility=hidden
```

Per-file overrides (in release builds):
- HOT (`-O3`): lexer, cp, grep, sed, sort, wc, find, cat, mkdir, chmod, charclass, linescan, arena, strbuf, intern, fscache, hashmap, thompson, classify, compile
- COLD (`-Os`): loader, registry, error, platform

**LDFLAGS (glibc dynamic):**
```
-pie
-Wl,--gc-sections -Wl,--as-needed
-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
-Wl,--build-id=sha1
```

**Header dependency tracking:** `-MMD -MP` (added 2026-03-30; generates `.d` files in `build/obj/`).

**CFLAGS_DEBUG:**
```
-O0 -g3 -fno-omit-frame-pointer
-fsanitize=address,undefined
```
(no LTO, no march, no fvisibility, no fno-unwind-tables)
