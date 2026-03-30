# Builtin-to-Builtin Pipe Elimination

**Date:** 2026-03-30
**Status:** Phase 1 implemented; Phase 2 deferred to v0.3.0

---

## Problem

Every stage in a pipeline (except the last) is run in a forked child process. A fork on Linux costs ~500 µs (page-table copy + kernel context switch). A pipeline like:

```sh
cat file.txt | grep pattern | wc -l
```

creates 2 fork() calls, 2 pipe() system calls, 2 sets of dup2() calls, and 2 waitpid() calls. When all the commands are builtins, this overhead dominates the actual work for small inputs.

---

## Phase 1: Trivial cat elimination (implemented, v0.2.0)

**Location:** `src/shell/exec.c`, `pipeline_elim_trivial_cat()`

**Pattern eliminated:**
```sh
cat | APPLET [args...]
```
Transformed to:
```sh
APPLET [args...]          # reads stdin directly
```

**When safe:**
- `cat` has no arguments (reads stdin and passes it through)
- `cat` has no flags (`-n`, `-v`, etc.)
- `cat` has no redirections

**Not handled** (left as normal pipeline):
- `cat FILE... | APPLET` — semantic difference: `APPLET FILE...` may add filename prefixes (grep -H)
- `cat -n | APPLET` — cat flags change output

**Savings per eliminated stage:** ~500 µs fork + pipe overhead.

**Example:**
```sh
# Before optimisation (2 stages → fork + pipe):
cat | grep "error"

# After optimisation (1 stage → no fork, no pipe):
grep "error"             # reads from original stdin
```

---

## Phase 2: Thread-based pipeline (planned, v0.3.0)

### Problem with current approach

Forking for each non-last pipeline stage is correct but expensive. For N-stage pipelines of pure applets, we create N-1 forks. With shell scripts that run hundreds of pipelines (e.g., build scripts), this adds up to tens of milliseconds of fork overhead.

### Proposed approach: per-stage pthread

For pipelines where ALL stages are "pure applets" (stateless, read-only applets from a known safe set), use POSIX threads instead of fork:

```
Stage 0 ──pipe[0]──> Stage 1 ──pipe[1]──> Stage 2 (current process)
 (thread)              (thread)             (inline)
```

Each intermediate stage runs in a `pthread` with:
- Its stdout `dup2`'d to the write end of its output pipe
- Its stdin `dup2`'d to the read end of its input pipe
- A global mutex serialising concurrent applet access (to protect non-thread-safe global state like `g_matched_any` in grep)

### Thread-safety constraints

The current applet code has several non-thread-safe elements:

| Issue | Location | Mitigation |
|-------|----------|------------|
| `g_matched_any` global | `src/core/grep.c:74` | Move to `grep_opts_t` (per-invocation state) |
| Static `grep_iobuf` | `src/core/grep.c:665` | Change to stack-allocated or per-call heap |
| `g_last_re`, `g_last_src` in sed | `src/core/sed.c` | Already per-invocation after UAF fix |
| Arena allocator (single-threaded) | `src/util/arena.c` | Use per-thread arenas (already stack-local in shell) |
| `isatty()` stdout check | grep, sed, sort | Thread-safe (read-only syscall) |

### "Pure applet" whitelist

Only these applets are safe to run in threads (stateless, no shell-state modification):

```
cat  grep  sed  sort  head  tail  wc  tr  cut  tee  uniq (if added)
```

Explicitly excluded (modify shell or process state):
```
cd  export  unset  exec  eval  source  trap  set  shift  read
```

### Estimated savings (for a 3-stage pipeline)

| Metric | Fork-based | Thread-based | Delta |
|--------|-----------|--------------|-------|
| fork() calls | 2 | 0 | −2 |
| pthread_create() calls | 0 | 2 | +2 |
| pipe() calls | 2 | 2 | 0 |
| Total latency (3-stage) | ~1.0 ms | ~0.1 ms | −0.9 ms |

Thread creation is ~50 µs vs fork ~500 µs on Linux (x86_64, glibc).

### Implementation plan (v0.3.0)

1. **Refactor `grep_opts_t`**: move `g_matched_any` into `grep_opts_t` (already per-call)
2. **Make `grep_iobuf` per-call**: stack-allocate (128 KB is too large for stack) or use `malloc` with `free` on exit
3. **Add `pipeline_all_pure(stages, nstages)`** predicate: checks each stage's first word against the pure-applet whitelist
4. **Thread wrapper**: `pipeline_run_threaded(sh, stages, nstages)` using `pthread_create` + `pthread_join`
5. **fd isolation**: each thread gets its own stdin/stdout via `dup()/dup2()` before calling the applet

### Constraints and risks

- **`dup2` is process-scoped**: modifying fd 0/1 in a thread affects all threads. Must save/restore with serialisation.
- **Global mutex approach** (first implementation): run threads sequentially with pipelined I/O. This preserves correctness while avoiding the fd-scoping issue. Full parallelism requires per-thread fd isolation.
- **Regression risk**: threading changes are sensitive. Must be gated by a new test suite (`tests/unit/shell/test_pipes.sh` extended).

---

## Phase 3: cat-file elimination (planned, v0.4.0)

```sh
cat FILE1 FILE2 | applet [args]  →  applet [args] FILE1 FILE2
```

Safe only for applets where multi-file input produces identical output to piped input, i.e. applets that:
1. Do NOT print filename prefixes with multiple file args
2. Treat multiple files as a single concatenated stream

Applets where this is safe:
- `wc` (with exactly one of `-l`, `-w`, `-c` but NOT multiple files with no flag since it prints "total")
- `head -n N` (takes first N lines of concatenated stream, same as multiple files in sequence)
- `tail -n N` — **NOT SAFE**: `tail -n 5 FILE1 FILE2` shows 5 lines from EACH file

Applets where it is **NOT SAFE**:
- `grep` — adds filename prefixes when given multiple file args
- `tail` — processes each file independently
- `wc` with no flags — prints "total" line
- `sort` — same output but different file-context semantics

Given these constraints, Phase 3 will only be implemented for `wc -l | ...` and similar narrow cases where the transformation is provably equivalent.

---

## Summary

| Phase | Pattern | Status | Savings |
|-------|---------|--------|---------|
| 1 | `cat \| applet` | **Done** | ~500 µs/pipeline |
| 2 | All-applet threaded pipeline | v0.3.0 | ~0.9 ms per 3-stage |
| 3 | `cat FILE... \| applet` | v0.4.0 | ~500 µs + IPC |
