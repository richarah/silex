# silex architecture

## Overview

silex is a single static binary containing a minimal POSIX shell and
coreutils as in-process builtins. Its primary goal is to speed up container
image builds by eliminating the fork/exec overhead of shell + coreutils for
every RUN step in a Dockerfile.

## Component diagram

```
+------------------------------------------------------------------+
|                        silex binary                           |
|                                                                  |
|  +--------------------+     +------------------------------+    |
|  |   multicall entry  |     |       POSIX shell            |    |
|  |   src/main.c       |---->|  src/shell/shell.c           |    |
|  |   argv[0] dispatch |     |  lexer -> parser -> exec     |    |
|  +--------------------+     +------------------------------+    |
|           |                          |                          |
|           |                          v                          |
|  +-------------------+    +------------------------+           |
|  |  applet_table[]   |    |  builtin short-circuit |           |
|  |  (27 builtins)    |<---|  check applet_table[]  |           |
|  +-------------------+    |  before fork+exec      |           |
|           |                +------------------------+           |
|           v                                                      |
|  +-------------------+    +---------------------+              |
|  |  src/core/        |    |  src/module/        |              |
|  |  cp, echo, mkdir  |    |  loader.c           |              |
|  |  cat, chmod, mv   |    |  registry.c         |              |
|  |  rm, ln, touch    |    |  dlopen + security  |              |
|  |  head, tail, wc   |    +---------------------+              |
|  |  sort, grep, sed  |                                          |
|  |  find, xargs, ... |    +---------------------+              |
|  +-------------------+    |  src/batch/         |              |
|                            |  io_uring batching  |              |
|  +-------------------+    |  independence detect|              |
|  |  src/util/        |    |  sequential fallback|              |
|  |  arena.c          |    +---------------------+              |
|  |  strbuf.c         |                                          |
|  |  path.c           |    +---------------------+              |
|  |  error.c          |    |  src/cache/         |              |
|  |  platform.c       |    |  fscache.c          |              |
|  +-------------------+    |  hashmap.c          |              |
|                            |  TTL invalidation   |              |
|                            +---------------------+              |
+------------------------------------------------------------------+
           |
           v
  modules/*.so  (optional, loaded via dlopen)
  cp_reflink.so, grep_pcre.so, sed_inplace.so, ...
```

## Data flow

```
Input (script file, -c string, or stdin)
    |
    v
lexer_t (src/shell/lexer.c)
    Tokenizes: words, operators, heredocs, quotes, expansions
    |
    v
parser_t (src/shell/parser.c)
    Builds AST: node_t tree (N_CMD, N_PIPE, N_IF, ...)
    |
    v
expand (src/shell/expand.c)
    Tilde, $VAR, $(), $(( )), field split, glob
    |
    v
exec_node (src/shell/exec.c)
    |
    +-- is builtin? --> applet_table[i].fn(argc, argv)  [no fork]
    |
    +-- is function? --> exec function body with new scope
    |
    +-- is external? --> fork() + execvp()
```

## Memory model

All per-command allocations use an arena allocator (`src/util/arena.h`).
The arena is reset between top-level commands (shell main loop), not freed.
This amortizes allocation overhead across the entire shell session.

Key arenas:
- `shell_ctx_t.scratch_arena`: reset after each top-level command; expansions
  for the command currently executing.
- Parse arenas: each script file, `eval`/trap string and interactive input
  line is parsed into a private arena freed when it finishes. (A script read
  from stdin is parsed into `shell_ctx_t.parse_arena`.)
- Function definitions: each one owns a small private arena holding a deep
  copy of its body, freed when the function is redefined or unset. The
  definition is reference counted, so a body that redefines or unsets the
  function that is running finishes the body it started with.
- Function-call variable scopes: a private arena per scope, freed on return.

Heap allocations (`malloc`), freed when the object goes away -- none of these
may live in a never-reset arena, or redefining them in a loop grows the shell
without bound:
- Global shell variables (entry, name, value); freed by `unset`
- The `NAME=value` strings the shell installs in `environ` for exported
  variables (via putenv; the previous string is freed on reassignment --
  glibc's setenv() never frees the old copy)
- Aliases, trap action strings
- Module registry entries (long-lived)
- Filesystem cache entries (long-lived, TTL-invalidated)
- Job list entries (freed on wait)
- Sort input lines (freed after output)
- Regex compiled patterns (freed at sed/grep exit)

## In-process applets and process-wide state

Inside `silex sh` an applet is a function call, so any process-wide state it
touches outlives it. The rules:
- Every applet resets its own globals at entry (grep's match flag, sed's
  options, xargs' -P table, ...) and re-reads the environment it depends on
  (`tzset()` for TZ).
- `simple_cmd_applet()` brackets every applet with `applet_stdio_begin/end`
  (`src/shell/exec.c`): stdin's stale buffer and EOF flag are dropped before
  it runs; afterwards unread buffered input is handed back to a seekable fd 0
  (`fflush` on an input stream, POSIX 2008), so `{ head -n1; cat; } <f` splits
  the file, and any buffering mode the applet chose for stdin/stdout (`sed -u`)
  is put back.

## Concurrency model

silex is single-threaded within a shell session. Concurrency arises from:

1. **Pipeline execution**: `fork()` per pipeline stage; parent waits for all
   children at the end of the pipeline.

2. **Background jobs**: `&` forks a child; parent continues; `job_reap()`
   non-blockingly harvests completed children.

3. **io_uring batching** (`src/batch/`): For sequences of independent
   filesystem operations (e.g., 100x `mkdir -p`), operations are submitted
   to a single io_uring submission queue and completed in bulk. The
   `batch_ops_independent()` detector ensures only safe batches use io_uring;
   all others fall through to sequential execution.

4. **xargs -P**: Multiple child processes for parallel arg processing.

## Module system

Modules are `.so` files loaded via `dlopen()`. Security checks on every load:
- Not a symlink
- Owned by root or the current user
- Not world-writable
- Module directory not world-writable
- libc tag in `silex_module_t.libc` must match the runtime build (musl or glibc)

The module registry caches `(tool, flag) -> so_path` mappings. The cache is
invalidated when the module directory's mtime changes.

Unsupported flags trigger module lookup before falling through to `execvp()`
of the external tool from `$PATH`.

## Code conventions

### Naming

| Entity | Convention | Example |
|--------|-----------|---------|
| Applet entry points | `applet_NAME()` | `applet_cp()`, `applet_grep()` |
| Internal helpers | `do_NAME()` | `do_copy_file()`, `do_sort_lines()` |
| Parsers / accessors | `parse_NAME()`, `get_NAME()` | `parse_flags()`, `get_next_line()` |
| Struct types | `name_t` suffix | `shell_ctx_t`, `strbuf_t`, `node_t` |
| Macros | ALL_CAPS | `ARENA_BLOCK_SIZE`, `SHELL_MAX_CALL_DEPTH` |
| File-level comment | `/* tool.c -- short description */` | first line of every .c file |

### Return values

All public functions return `int` representing an exit code:
- 0: success
- 1 or greater: error (propagated as the process exit status)
- 2: usage error (invalid arguments; print usage to stderr before returning)

Shell builtins (`applet_*`) follow the same convention. Internal helpers that
cannot fail return void. Functions that return pointers return NULL on failure
and print to stderr before returning.

### Option parsing

Applets with ordinary flags parse them with the shared iterator in
`src/util/opt.h` (not getopt: its global `optind` state would leak between
in-process builtin runs). It walks argv once, keeps all state in a stack
`opt_t`, and never allocates. It only classifies arguments; every message and
exit code stays with the applet. Not covered: echo, printf, find, sh, env,
xargs and loadable modules, whose syntax is their own.

### Memory

- Per-command allocations use `arena_alloc()` from `src/util/arena.h`.
- Long-lived allocations (module registry, fscache, sort lines) use `malloc()`.
- Every `malloc()`/`realloc()` return is checked; NULL causes an error message
  and exit(1) via `err_oom()` in `src/util/error.h`.
- `strbuf_t` (`src/util/strbuf.h`) is used for all variable-length strings.
  Direct `char[]` arrays with user-controlled sizes are forbidden.

### Visibility

All non-API symbols are compiled with `-fvisibility=hidden` in release builds.
Symbols that must be visible across translation units are declared in their
respective header files. Module `.so` exports must use `SILEX_EXPORT`
(defined in `silex_module.h`) to override hidden visibility.
