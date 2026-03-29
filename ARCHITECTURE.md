# matchbox Architecture

## Overview

matchbox is a single static binary containing a minimal POSIX shell and
coreutils as in-process builtins. Its primary goal is to speed up container
image builds by eliminating the fork/exec overhead of shell + coreutils for
every RUN step in a Dockerfile.

## Component Diagram

```
+------------------------------------------------------------------+
|                        matchbox binary                           |
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

## Data Flow

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

## Memory Model

All per-command allocations use an arena allocator (`src/util/arena.h`).
The arena is reset between top-level commands (shell main loop), not freed.
This amortizes allocation overhead across the entire shell session.

Key arenas:
- `shell_ctx_t.parse_arena`: reset after each top-level command; holds all
  AST nodes, token strings, word arrays from the last parsed command.
- `vars_t.arena`: never reset during session; holds variable names and values.
  Variables are updated by creating new arena_strdup entries.

Heap allocations (`malloc`):
- Module registry entries (long-lived)
- Filesystem cache entries (long-lived, TTL-invalidated)
- Job list entries (freed on wait)
- Sort input lines (freed after output)
- Regex compiled patterns (freed at sed/grep exit)

## Concurrency Model

matchbox is single-threaded within a shell session. Concurrency arises from:

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

## Module System

Modules are `.so` files loaded via `dlopen()`. Security checks on every load:
- Not a symlink
- Owned by root or the current user
- Not world-writable
- Module directory not world-writable

The module registry caches `(tool, flag) -> so_path` mappings. The cache is
invalidated when the module directory's mtime changes.

Unsupported flags trigger module lookup before falling through to `execvp()`
of the external tool from `$PATH`.
