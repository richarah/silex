# silex — Container Build Runtime

## What this is

silex is a purpose-built container build runtime: a single static binary containing a minimal POSIX shell and common coreutils as builtins, optimised for the container build hot path. It is not a general-purpose userspace. It exists to make `RUN` steps in Dockerfiles faster.

It is part of the Silex ecosystem but ships as a standalone binary you can drop into any container.

## Core principles

1. The common case must be fast. cp, mv, rm, mkdir, chmod, cat with basic flags must execute as shell builtins with zero fork+exec overhead.
2. Rare GNU flags must work. When the argument parser encounters a flag the core does not handle, it dlopen's a module that does. Full GNU coreutils compatibility is the goal.
3. One binary. Statically linked with musl. No runtime dependencies. Drop it in, symlink tool names to it, done.
4. Security is non-negotiable. No buffer overflows, no TOCTOU races, no injection vectors. Every string operation is bounds-checked. Every path is validated.

## Architecture

### Base: toybox fork

Fork toybox (BSD licensed, clean C, multicall binary). Use its applet structure as the skeleton. Strip everything interactive (login, getty, terminal handling). Keep the core file/text utilities. Modify the build system to support our module loading and shell integration.

Toybox repo: https://github.com/landley/toybox
Use the latest stable tag at time of development.

### Multicall binary

The compiled binary is called `silex`. It determines which tool to run by examining argv[0]:

```
ln -s /usr/local/bin/silex /usr/local/bin/cp
ln -s /usr/local/bin/silex /usr/local/bin/mv
ln -s /usr/local/bin/silex /usr/local/bin/sh
# etc.
```

`silex --install /usr/local/bin` creates all symlinks automatically.
`silex --list` prints all available applets.

### Language

C. The core, the shell, the builtins, and the module interface are all C. This is for compatibility with toybox, for minimal binary size, and for predictable performance. No C++ in the core binary. Modules may be written in C or C++ but must expose a C ABI.

Use C11 standard. Use `-Wall -Wextra -Werror -pedantic` at minimum.

### Build system

GNU Make. Not CMake, not meson. A single Makefile that:

- Builds the static silex binary linked against musl
- Builds all modules as .so files
- Runs the test suite
- Supports cross-compilation for x86_64 and aarch64
- Supports `make install PREFIX=/usr/local`
- Supports `make silex-static` for fully static binary with no module support (embedded use)

Compiler: clang preferred, gcc supported. Use `-O2 -flto` for release builds. Use `-fsanitize=address,undefined` for debug builds.

### Static linking

The main binary MUST be statically linked against musl libc. The module loader (dlopen) is the ONLY dynamic linking in the system and it is optional; if compiled with `SILEX_STATIC=1`, module loading is disabled entirely and all unrecognised flags print a clear error message suggesting the user install the relevant module or GNU coreutils.

---

## Component 1: Minimal POSIX shell

A POSIX-compliant `/bin/sh` replacement for script execution only.

### What it includes

- POSIX sh syntax: pipes, redirects, here-docs, command substitution, arithmetic expansion, parameter expansion (including `${var##pattern}`, `${var%%pattern}`, `${var:-default}`, `${var:+alt}`, `${var:=default}`, `${var:?error}`)
- Control flow: if/elif/else/fi, for/do/done, while/until, case/esac
- Functions
- Subshells via `()`
- Logical operators: `&&`, `||`, `!`
- Background execution: `&`, `wait`
- Signal handling: `trap`
- Builtins: cd, echo, test, [, export, unset, exec, exit, set, shift, read, trap, . (source), printf, true, false, :, eval, command, type, local, return, break, continue, getopts, umask, readonly

### What it does NOT include

- Readline / line editing (no interactive use in containers)
- Command history
- Tab completion
- Job control (no fg, bg, jobs)
- Bash-isms in the core: no `[[`, no arrays, no process substitution, no `<<<` here-strings, no `{a,b}` brace expansion
- PS1/PS2 prompt handling beyond minimal

### Bash-ism modules

When the parser encounters bash syntax that POSIX sh does not support, check for a loaded module:

- `module_bash_arrays.so` — arrays, associative arrays
- `module_bash_test.so` — `[[` extended test syntax
- `module_bash_proc.so` — process substitution `<()`, `>()`
- `module_bash_misc.so` — here-strings `<<<`, brace expansion `{a..z}`

If the module is not present, print: `silex: syntax not supported in POSIX mode. Install silex-bash module for bash compatibility.`

### Performance target

- Shell startup to first command execution: <1ms
- Simple script (`#!/bin/sh\necho hello`): <2ms total wall time
- Measure against dash, which is the current fastest POSIX sh

---

## Component 2: Coreutils as shell builtins

When the shell executes a command that matches a builtin applet AND the flags are within the builtin's supported set, execute it in-process. No fork, no exec.

### Builtin applets (execute in-process for simple flags)

Each builtin handles the POSIX-specified flags plus the most commonly used GNU flags. Any flag outside this set triggers the module loader.

```
cp      — POSIX: -r, -R, -p, -f, -i, -L, -H, -P
          Common GNU: -a (alias for -rpP), -v, -n, -u, -T
mv      — POSIX: -f, -i
          Common GNU: -v, -n, -u, -T
rm      — POSIX: -r, -R, -f, -i
          Common GNU: -v, --no-preserve-root (reject with error)
mkdir   — POSIX: -p, -m
          Common GNU: -v
chmod   — POSIX: -R, octal and symbolic modes
          Common GNU: -v, --reference
cat     — POSIX: -u
          Common GNU: -n, -b, -s, -v, -e, -t
ln      — POSIX: -s, -f
          Common GNU: -v, -n, -r, -T
touch   — POSIX: -a, -m, -c, -t, -r
          Common GNU: -d
head    — POSIX: -n
          Common GNU: -c, -q
tail    — POSIX: -n, -f
          Common GNU: -c, -q, --pid
wc      — POSIX: -c, -l, -w
          Common GNU: -m, -L
sort    — POSIX: -b, -d, -f, -i, -n, -r, -u, -o, -t, -k
          Common GNU: -s, -g, -h, -V, -z
grep    — POSIX: -E, -F, -c, -i, -l, -n, -q, -s, -v, -e, -f
          Common GNU: -r, -R, -w, --include, --exclude, --color
sed     — POSIX: -n, -e, -f, s///, d, p, q, y
          Common GNU: -i (in-place without backup), -E/-r, w
basename — POSIX: standard
dirname  — POSIX: standard
readlink — -f (canonicalize)
stat    — Both GNU --format and busybox -c syntax supported
date    — POSIX and GNU --date parsing
printf  — POSIX standard
install — -d, -m, -o, -g
find    — POSIX: -name, -type, -exec, -print, -maxdepth, -mindepth
          Common GNU: -iname, -delete, -empty
xargs   — POSIX: -I, -n, -P
          Common GNU: -0, -r, -d
```

### Builtin execution flow

```
1. Shell parses command line
2. Command name matches a builtin applet?
   YES → parse flags
         All flags in builtin supported set?
         YES → execute in-process, no fork
         NO  → check module loader for the unsupported flag
               Module found? → dlopen, call module handler
               Module not found? → fall through to external command (fork+exec from PATH)
   NO  → fork+exec from PATH as normal
```

### Critical rule

Builtins MUST produce byte-identical output to GNU coreutils for the same input and flags. Not "similar output." Identical. Test this with diff against GNU output in the test suite.

---

## Component 3: Module system

### Module directory

Default: `/usr/lib/silex/modules/`

SILEX_MODULE_PATH environment variable can add ADDITIONAL directories (appended, not replacing the default). The default directory is always checked first.

### Security constraints on modules

- Module .so files MUST be owned by root (uid 0) or the user running silex
- Module .so files MUST NOT be world-writable
- Module directory MUST NOT be world-writable
- Symlinks in the module path are NOT followed (use realpath and verify)
- These checks happen on every dlopen. No caching of security checks.

### Module interface

```c
// silex_module.h — public header

#include <stddef.h>

#define SILEX_MODULE_API_VERSION 1

typedef struct {
    int api_version;                    // must be SILEX_MODULE_API_VERSION
    const char *tool_name;              // e.g., "cp"
    const char *module_name;            // e.g., "cp_gnu_reflink"
    const char *description;            // human-readable, for --help
    const char **extra_flags;           // NULL-terminated: {"--reflink", "--sparse", NULL}
    int (*handler)(int argc, char **argv, int flag_index);
    // flag_index: index into argv where the unrecognised flag was found
    // returns: 0 on success, >0 on error (propagated as exit code)
} silex_module_t;

// Every module .so exports this symbol:
silex_module_t *silex_module_init(void);
```

### Module naming convention

`<tool>_<feature>.so` — e.g., `cp_reflink.so`, `sort_parallel.so`, `sed_inplace.so`

### Module loading flow

```
1. Builtin encounters unsupported flag "--reflink" for tool "cp"
2. Scan module directory for files matching "cp_*.so"
3. For each: dlopen, call silex_module_init(), check extra_flags list
4. Found match? → call handler with full argc/argv
5. No match? → fall through to fork+exec of external "cp" from PATH
6. No external "cp" in PATH? → print error: "silex: cp: unsupported flag --reflink. Install silex-gnu-cp module or GNU coreutils."
```

### Module caching

After first scan of the module directory, cache the mapping of (tool_name, flag) → module .so path in memory. Invalidate if module directory mtime changes. This avoids repeated directory scans.

---

## Component 4: Syscall batching with io_uring

### When to batch

The shell analyses the current command sequence. If it detects multiple independent filesystem operations (no shared paths, no data dependencies between them), it submits them as a single io_uring batch.

### Independence detection

Two operations are independent if:
- They operate on non-overlapping paths (no shared prefixes beyond the root or a common parent that is only read, not written)
- Neither reads output of the other
- Neither depends on the existence of a file/directory created by the other

Conservative analysis: if in doubt, do NOT batch. Sequential execution is always correct. Batching is an optimisation, not a requirement.

### Supported batched operations

- `mkdir -p` sequences (common in Dockerfiles: `mkdir -p /app /data /config /logs`)
- `chmod` sequences on independent paths
- `cp` of independent files to independent destinations
- `rm` of independent paths
- `touch` sequences

### Fallback

If io_uring is not available (kernel < 5.1, or seccomp blocks it, common in some container runtimes), fall back to sequential execution silently. No error, no warning. Check once at startup, cache the result.

### io_uring safety

- Use IORING_SETUP_SQPOLL only if running as root. Otherwise use standard submission.
- Limit queue depth to 256 entries to prevent resource exhaustion.
- All submitted operations are validated before submission: no symlink following, no path traversal, all paths resolved to absolute.

---

## Component 5: Filesystem state cache

### Purpose

Avoid redundant stat() syscalls. Docker build scripts frequently check if a file/directory exists before creating or modifying it. The cache tracks what the shell process has already learned about the filesystem.

### Implementation

A hash map keyed by (device, inode) pairs. Values: file type (regular, directory, symlink, other), size, mtime, mode. Populated on every stat/lstat/open/mkdir/etc. that the shell or builtins perform.

### Invalidation rules

- Any write operation (create, delete, rename, chmod, chown, truncate, write) on a path invalidates that path's cache entry AND its parent directory's entry.
- Any operation on a path not performed by silex itself (detected via inotify if available, or just invalidate conservatively) is not tracked. The cache is ONLY for operations silex itself performs.
- Cache is per-shell-process. Not shared between processes. Not persisted to disk.
- Cache entries expire after 5 seconds as a safety net. This is configurable via SILEX_FSCACHE_TTL=0 to disable entirely.

### Safety rule

The cache MUST NEVER cause a different observable result than not having the cache. If there is any doubt, invalidate. A cache miss costs one stat() call. A cache corruption costs a broken build.

---

## Component 6: Built-in string operations

### Purpose

Eliminate fork+exec for common string manipulations that Dockerfiles do via piping to sed, awk, basename, dirname, cut, tr.

### Builtins

basename and dirname are already listed as builtin applets above. Additionally:

The shell's parameter expansion handles:
- `${var#pattern}`, `${var##pattern}` — prefix strip (replaces many sed/cut uses)
- `${var%pattern}`, `${var%%pattern}` — suffix strip
- `${var/pattern/replacement}` — first match replace (POSIX extension, widely supported)
- `${var//pattern/replacement}` — global replace

The built-in sed handles simple cases in-process:
- `sed 's/pattern/replacement/'` and `sed 's/pattern/replacement/g'`
- `sed '/pattern/d'`
- `sed -n '/pattern/p'`

If the sed expression is more complex (multi-line, holds space, labels, branching), fall through to external sed or a module.

The built-in tr handles:
- Character class translation: `tr 'a-z' 'A-Z'`
- Character deletion: `tr -d '\n'`
- Squeeze: `tr -s ' '`

The built-in cut handles:
- `-d` delimiter and `-f` field selection

### Detection of simple vs complex cases

The parser examines the sed/tr/cut expression BEFORE deciding whether to run it in-process. If the expression uses features beyond the built-in set, fork+exec to the real tool. Never partially execute a complex expression in-process.

---

## File structure

```
silex/
├── README.md
├── LICENSE                  (BSD 2-clause)
├── Makefile
├── silex_module.h        (public module API header)
├── src/
│   ├── main.c               (multicall entry, argv[0] dispatch)
│   ├── shell/
│   │   ├── shell.c           (shell main loop)
│   │   ├── lexer.c           (tokeniser)
│   │   ├── parser.c          (AST builder)
│   │   ├── exec.c            (command execution, builtin dispatch, fork+exec)
│   │   ├── expand.c          (variable expansion, globbing, tilde expansion)
│   │   ├── redirect.c        (file descriptor redirection)
│   │   └── job.c             (minimal: wait, background, no job control)
│   ├── core/
│   │   ├── cp.c
│   │   ├── mv.c
│   │   ├── rm.c
│   │   ├── mkdir.c
│   │   ├── chmod.c
│   │   ├── cat.c
│   │   ├── ln.c
│   │   ├── touch.c
│   │   ├── head.c
│   │   ├── tail.c
│   │   ├── wc.c
│   │   ├── sort.c
│   │   ├── grep.c
│   │   ├── sed.c
│   │   ├── find.c
│   │   ├── xargs.c
│   │   ├── basename.c
│   │   ├── dirname.c
│   │   ├── readlink.c
│   │   ├── stat.c
│   │   ├── date.c
│   │   ├── install.c
│   │   ├── tr.c
│   │   ├── cut.c
│   │   └── printf.c
│   ├── module/
│   │   ├── loader.c           (dlopen, security checks, caching)
│   │   └── registry.c         (flag→module mapping cache)
│   ├── batch/
│   │   ├── uring.c            (io_uring wrapper)
│   │   ├── detect.c           (independence analysis)
│   │   └── fallback.c         (sequential fallback)
│   ├── cache/
│   │   ├── fscache.c          (filesystem state cache)
│   │   └── hashmap.c          (hash map implementation)
│   └── util/
│       ├── arena.c            (arena allocator for parse trees, temp strings)
│       ├── strbuf.c           (bounds-checked string buffer)
│       ├── path.c             (path canonicalisation, validation)
│       ├── error.c            (error reporting)
│       └── platform.c         (feature detection: io_uring, inotify, etc.)
├── modules/
│   ├── cp_reflink.c
│   ├── cp_sparse.c
│   ├── cp_backup.c
│   ├── sort_parallel.c
│   ├── sed_inplace.c
│   ├── grep_pcre.c
│   ├── stat_format.c
│   ├── date_gnu.c
│   ├── sha256sum_tag.c
│   └── bash_arrays.c
├── tests/
│   ├── unit/                   (per-function tests)
│   ├── compat/                 (GNU output compatibility tests)
│   ├── integration/            (real Dockerfile builds)
│   ├── security/               (module loading, path traversal, injection)
│   ├── bench/                  (benchmarks against busybox, GNU, dash)
│   └── fuzz/                   (AFL/libFuzzer targets for parser, builtins)
├── docker/
│   ├── Dockerfile.build        (build silex itself in a clean environment)
│   ├── Dockerfile.test         (test silex in a real container build)
│   └── Dockerfile.bench        (benchmark container)
└── scripts/
    ├── gen-symlinks.sh          (generate symlink install script)
    ├── bench.sh                 (run benchmarks)
    └── compat-test.sh           (run GNU compatibility tests)
```

---

## Testing requirements

### Unit tests

Every builtin applet has unit tests covering:
- Every supported flag individually
- Flag combinations
- Edge cases: empty input, huge input, binary input, unicode, paths with spaces and special characters, paths at PATH_MAX length, filenames starting with `-`
- Error conditions: permission denied, file not found, disk full, read-only filesystem
- Output is byte-identical to GNU coreutils for the same input

### GNU compatibility tests

For every builtin applet, run the same command with silex and with GNU coreutils. Diff the stdout, stderr, and exit code. Any difference is a bug.

Script: `tests/compat/run.sh` takes a file of test commands, runs each with silex and GNU, reports differences.

### Integration tests

Real Dockerfiles from popular projects (node, python, golang, rust base images). Build with silex as /bin/sh. Build must succeed and produce identical image contents as building with dash/bash.

### Security tests

- Module loading from a world-writable directory: must refuse
- Module .so with wrong ownership: must refuse
- Path traversal in module path: must refuse
- Symlink following in module path: must refuse
- Buffer overflow attempts in flag parsing: must not crash (fuzz test)
- Malicious shell scripts: must not allow escape from intended execution

### Fuzz tests

AFL or libFuzzer targets for:
- Shell lexer (arbitrary input)
- Shell parser (arbitrary token sequences)
- Each builtin's argument parser
- Module loader path handling
- sed expression parser
- grep pattern parser

### Benchmarks

`tests/bench/` contains:
- `bench_startup.sh` — measure shell startup time (1000 iterations of `silex -c "true"`)
- `bench_builtins.sh` — measure builtin vs fork+exec for each applet
- `bench_batch.sh` — measure io_uring batching vs sequential for mkdir/chmod sequences
- `bench_dockerfile.sh` — measure real Dockerfile build times
- `bench_cache.sh` — measure filesystem cache hit rate and speedup

Compare against: dash, busybox ash, bash, GNU coreutils.
Output as TSV for easy graphing.

---

## Performance targets

| Operation | Target | Comparison |
|-----------|--------|------------|
| Shell startup | <1ms | dash ~1ms, bash ~4ms |
| Builtin cp (small file) | <0.5ms | fork+exec cp ~3ms |
| Builtin mkdir -p | <0.3ms | fork+exec mkdir ~2ms |
| 100x mkdir -p (batched) | <5ms | 100x fork+exec ~200ms |
| sed s/foo/bar/ (builtin) | <0.2ms | fork+exec sed ~3ms |
| Real Dockerfile build | 2-5x faster | vs bash+GNU on same Dockerfile |
| Binary size (static, no modules) | <1MB | busybox ~1MB, toybox ~800KB |

---

## Security requirements summary

1. All string operations use strbuf (bounds-checked, never raw char* manipulation for user input)
2. All paths canonicalised and validated before use (no ../ traversal, no symlink following in sensitive contexts)
3. No use of alloca() with sizes derived from user input
4. No use of system() or popen() inside builtins (defeats the purpose and is unsafe)
5. Module .so files validated (ownership, permissions, no symlinks) before dlopen
6. Module directory validated (ownership, permissions) before scanning
7. io_uring submissions validated (paths resolved, no symlinks in batch ops)
8. Arena allocator for all temporary allocations during parse/execute (no leak accumulation in long-running shell)
9. ASLR and stack protector enabled in release builds (-fstack-protector-strong, -fpie)
10. All compiler warnings treated as errors
11. Fuzz testing integrated into CI

---

## Implementation order

This is the order in which to build silex. Each phase has a verification gate. Do NOT proceed to the next phase until the current phase passes its gate.

### Phase 1: Multicall skeleton + 3 builtins

1. Set up the repo structure
2. Implement main.c with argv[0] dispatch
3. Implement echo, mkdir, cp (POSIX flags only)
4. Implement `silex --install` and `silex --list`
5. Write Makefile for static build with musl
6. Write unit tests for the 3 builtins

**Gate:** `silex --install /tmp/test && /tmp/test/mkdir -p /tmp/test/a/b/c && /tmp/test/cp /etc/hostname /tmp/test/a/ && /tmp/test/echo "hello"` works. Unit tests pass. Output matches GNU.

### Phase 2: Minimal shell

1. Implement lexer, parser, exec for POSIX sh subset
2. Support: pipes, redirects, variables, command substitution, if/for/while/case, functions
3. Builtins (cd, export, etc.) wired into exec
4. Shell executes builtins in-process, forks for external commands
5. Write shell unit tests and integration tests

**Gate:** a real-world shell script (e.g., Alpine's `/etc/init.d/networking`) runs correctly. `tests/compat/posix-sh-tests.sh` passes.

### Phase 3: Remaining core builtins

1. Implement all builtins listed above with their POSIX + common GNU flags
2. Unit tests for each
3. GNU compatibility tests for each

**Gate:** all unit tests pass. All compat tests show byte-identical output to GNU for supported flags.

### Phase 4: Module system

1. Implement module loader with security checks
2. Implement module registry/cache
3. Implement 3 example modules: cp_reflink.so, sed_inplace.so, grep_pcre.so
4. Implement fallback-to-PATH when no module found
5. Write security tests for module loading

**Gate:** `silex cp --reflink=auto a b` loads cp_reflink.so and works. Module from a world-writable directory is rejected. Missing module falls through to GNU cp in PATH.

### Phase 5: Syscall batching

1. Implement io_uring wrapper
2. Implement independence detection for command sequences
3. Implement fallback for kernels without io_uring
4. Write benchmarks

**Gate:** `mkdir -p a b c d e f g h i j` is measurably faster with batching enabled than disabled. Fallback works on a kernel without io_uring.

### Phase 6: Filesystem cache

1. Implement hash map
2. Implement cache with invalidation rules
3. Implement TTL expiry
4. Write tests proving cache never causes different results than no cache

**Gate:** a build script that does repeated stat checks is measurably faster with cache. A test that creates a file via an external process and immediately checks for it via silex does NOT get a stale cache hit.

### Phase 7: Built-in string operations

1. Implement in-process sed for simple cases
2. Implement in-process tr and cut
3. Implement detection of simple vs complex expressions
4. Write tests

**Gate:** `echo "hello world" | silex sed 's/hello/goodbye/'` runs in-process (verify via strace: no fork). `echo "hello" | silex sed '1{h;d};/pattern/{x;p}' ` forks to external sed (complex expression).

### Phase 8: Integration and benchmarks

1. Build a real Docker image using silex as /bin/sh
2. Run benchmarks against busybox, dash, bash, GNU
3. Write README with results
4. Publish

**Gate:** at least 2x speedup on a real Dockerfile build. All tests pass. All compat tests pass. No security test failures.

---

## Explicit instructions for the agent

- Before writing any code, read the relevant toybox source file to understand its structure. Do not guess.
- Every function that handles user input MUST use strbuf, not raw char arrays.
- Every path MUST be canonicalised with realpath() before use in filesystem operations.
- Every file MUST have a header comment stating its purpose in one line.
- No global mutable state except: the module cache, the filesystem cache, and the io_uring state. These three are clearly isolated in their own translation units.
- No heap allocation in the hot path of builtin execution. Use the arena allocator.
- Test before proceeding. After implementing each builtin, run its tests before implementing the next.
- If a test fails, fix it before writing any new code.
- Commit after each phase gate passes, with a message describing what the phase achieved.
- If something is unclear in this prompt, make the conservative choice (slower but correct) and add a TODO comment explaining the ambiguity.

---

## What this is NOT

- Not a bash replacement. It is a POSIX sh with optional bash compatibility modules.
- Not a general-purpose userspace. It does not include: init, login, getty, networking tools, package management, editor, anything interactive.
- Not a security sandbox. It runs with whatever permissions the container gives it.
- Not a build system. It does not replace make, ninja, or cmake. It is the shell and coreutils that those build systems invoke.

---

## License

BSD 2-clause. Same as toybox. This allows commercial use, modification, and distribution with minimal restrictions. Modules may have their own licenses but the core MUST remain BSD.
