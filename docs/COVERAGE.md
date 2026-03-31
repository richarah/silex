# silex Code Coverage

**Generated:** 2026-03-30
**Version:** 0.2.0
**Method:** `gcc --coverage -g -O0` + `gcov`

## Test suites used

| Suite | Command | Tests |
|-------|---------|-------|
| Unit (C) | `make test` | 111 regex + misc |
| Compat | `make compat-test` | 54 TAP |
| Integration | `make integration-test` | 36 |
| Security | `make security-test` | 22 |
| Shell conformance | `tests/unit/shell/test_*.sh` | 203 |

---

## Shell subsystem

| File | Lines | Covered |
|------|-------|---------|
| vars.c | 140 | **92.86%** |
| parser.c | 591 | **89.34%** |
| expand.c | 808 | **80.57%** |
| lexer.c | 435 | **78.85%** |
| exec.c | 1011 | **71.12%** |
| shell.c | 131 | **70.99%** |
| job.c | 41 | **56.10%** |
| redirect.c | 164 | **54.88%** |
| **Subsystem total** | **3321** | **~78%** |

The shell subsystem has the highest coverage because all 203 shell conformance tests exercise it directly. Uncovered lines are primarily in background job control (`job.c` fd management) and rare redirect forms.

---

## Regex engine

| File | Lines | Covered |
|------|-------|---------|
| classify.c | 61 | **72.13%** |
| thompson.c | 220 | **70.91%** |
| compile.c | 35 | **65.71%** |
| mb_regex.c | 106 | **44.34%** |
| parse.c | 402 | **46.77%** |
| charclass_re.c | 97 | **0.00%** |
| **Subsystem total** | **921** | **~50%** |

`charclass_re.c` (0%): POSIX bracket expressions `[[:alpha:]]` etc. — exercised only when the charclass module is loaded at runtime. `parse.c` and `mb_regex.c` gaps: complex alternation, backreference, and named-group paths not exercised by existing grep/sed tests.

---

## Core builtins

### Actively exercised by test suites

| File | Lines | Covered | Notes |
|------|-------|---------|-------|
| wc.c | 148 | **89.86%** | Compat + integration |
| echo.c | 84 | **86.90%** | Compat + shell tests |
| mkdir.c | 116 | **78.45%** | Unit tests |
| dirname.c | 55 | **70.91%** | Compat |
| cp.c | 253 | **65.61%** | Unit tests |
| sh.c | 98 | **61.22%** | Shell conformance |
| rm.c | 134 | **55.97%** | Security test suite |
| basename.c | 76 | **55.26%** | Compat |
| cut.c | 215 | **48.84%** | Integration |
| cat.c | 231 | **40.26%** | Compat |
| sed.c | 822 | **40.15%** | Compat |
| tr.c | 165 | **35.76%** | Integration |
| grep.c | 546 | **33.88%** | Compat |
| sort.c | 575 | **29.39%** | Compat |
| head.c | 201 | **31.34%** | Integration |

### Not covered by current test suites (0%)

| File | Lines | Reason |
|------|-------|--------|
| chmod.c | 249 | No unit tests; compat suite omits chmod |
| date.c | 224 | No compat tests for date formatting |
| env.c | 114 | New in v0.2.0; no compat tests yet |
| find.c | 547 | No compat tests; integration tests use grep/sed |
| install.c | 287 | No compat tests |
| ln.c | 190 | No compat tests |
| mktemp.c | 66 | New in v0.2.0; no compat tests yet |
| mv.c | 189 | No compat tests |
| printf.c | 230 | No compat tests (printf is a shell builtin tested differently) |
| readlink.c | 109 | No compat tests |
| realpath.c | 133 | New in v0.2.0; no compat tests yet |
| sha256sum.c | 167 | New in v0.2.0; no compat tests yet |
| stat.c | 451 | No compat tests |
| tail.c | 324 | No compat tests |
| tee.c | 56 | New in v0.2.0; no compat tests yet |
| touch.c | 190 | No compat tests |
| xargs.c | 389 | No compat tests |

**Core subsystem total:** ~7,634 lines, **~22% covered** (low due to the 17 uncovered builtins above)

---

## Utilities

| File | Lines | Covered |
|------|-------|---------|
| linescan_avx2.c | 14 | **100.00%** |
| error.c | 27 | **100.00%** |
| arena.c | 50 | **82.00%** |
| strbuf.c | 61 | **68.85%** |
| intern.c | 89 | **55.06%** |
| path.c | 94 | **18.09%** |
| platform.c | 17 | **0.00%** |

`platform.c` (0%): io_uring and inotify detection — the coverage binary does not run as root and `SILEX_FORCE_FALLBACKS` skips detection entirely during security tests. `path.c` (18%): `path_canon()` (full `realpath`) is exercised only by rm safety tests; `path_normalize()` lexical path is rarely triggered by test scripts.

---

## Module system, batch I/O, cache, main

| File | Lines | Covered |
|------|-------|---------|
| fscache.c | 95 | **29.47%** |
| main.c | 81 | **25.93%** |
| hashmap.c | 74 | **2.70%** |
| loader.c | 78 | **0.00%** |
| registry.c | 128 | **0.00%** |
| uring.c | 106 | **0.00%** |
| detect.c | 30 | **0.00%** |
| fallback.c | 38 | **0.00%** |

- **Module loader/registry (0%):** module loading requires `.so` files in `/usr/lib/silex/modules/`. Test environment has no installed modules.
- **io_uring / inotify (0%):** not exercised in the coverage environment (rootless, no CAP_SYS_RAWIO; falls back gracefully via `SILEX_FORCE_FALLBACKS`).
- **fscache (29%):** basic stat caching exercised by integration tests; TTL invalidation and mtime paths not triggered.

---

## Overall summary

| Subsystem | Lines | Coverage |
|-----------|-------|----------|
| Shell | 3,321 | ~78% |
| Regex engine | 921 | ~50% |
| Utilities | 352 | ~60% |
| Core builtins (exercised) | 4,004 | ~57% |
| Core builtins (0% coverage) | 3,630 | 0% |
| Module / batch / cache / main | 636 | ~8% |
| **Total** | **~12,864** | **~40%** |

### Coverage improvement priorities

1. **Add compat tests for new v0.2.0 builtins** (`env`, `mktemp`, `tee`, `realpath`, `sha256sum`) — high-value, low-effort
2. **Add compat tests for file operation builtins** (`chmod`, `mv`, `ln`, `touch`, `install`, `tail`, `find`) — large uncovered surface
3. **Exercise charclass_re.c** via grep bracket expression tests (`grep '[[:alpha:]]'`)
4. **Module loading** requires test infrastructure: build a minimal `.so`, run with `SILEX_MODULE_PATH`
5. **io_uring / fallback paths** require `SILEX_FORCE_FALLBACKS=0` with appropriate kernel support

### How to regenerate

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DSILEX_VERSION=\"0.2.0\" -fstack-protector-strong -DSILEX_LIBC_GLIBC=1 --coverage -g -O0' LDFLAGS='-pie --coverage'
make test && make compat-test && make integration-test && make security-test
for f in tests/unit/shell/test_*.sh; do sh "$f" build/bin/silex; done
gcov -o build/obj/core build/obj/core/*.o
gcov build/obj/shell/*.o build/obj/util/*.o build/obj/util/regex/*.o
gcov build/obj/module/*.o build/obj/batch/*.o build/obj/cache/*.o build/obj/main.o
make clean  # restore normal build
make
```
