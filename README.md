# matchbox

A purpose-built container build runtime: a single static binary containing a minimal POSIX shell and 32 coreutils as builtins, optimised for the container build hot path.

matchbox makes `RUN` steps in Dockerfiles faster by executing common commands in-process — no fork, no exec, no dynamic-linking overhead for every `cp`, `mkdir`, or `echo`.

## Quick start

```sh
make
./build/bin/matchbox --install /usr/local/bin
```

This installs symlinks so `cp`, `echo`, `mkdir`, etc. all invoke the matchbox binary.

## Building

**Container images (musl static, smallest binary):**
```sh
make release          # requires musl-gcc; see release-docker on glibc hosts
make release-docker   # builds inside Alpine via Docker (no local musl-gcc needed)
```

**For glibc-based images (Debian, Ubuntu, Silex):**
```sh
make release-glibc
```

**Development (ASan/UBSan):**
```sh
make debug
make test
```

Both `release` and `release-glibc` produce `build/bin/matchbox` with identical features.
The musl static build is stripped and self-contained; the glibc dynamic build links
against the host libc.

**Architecture note:** x86-64 binaries require AVX2 (Intel Haswell 2013+,
AMD Excavator 2015+). For older hardware:
```sh
make release MARCH=x86-64-v2
```

Requires: C11 compiler (gcc or clang), GNU Make, linux-headers.

## Builtins (v0.2.0 — 32 applets)

| Category | Applets |
|----------|---------|
| File ops | `cp`, `mv`, `rm`, `ln`, `mkdir`, `install`, `touch`, `chmod` |
| Text | `cat`, `echo`, `printf`, `head`, `tail`, `grep`, `sed`, `sort`, `cut`, `tr`, `wc` |
| System | `stat`, `readlink`, `realpath`, `find`, `xargs`, `date`, `basename`, `dirname` |
| New (v0.2.0) | `env`, `mktemp`, `tee`, `realpath`, `sha256sum` |
| Shell | `sh` (POSIX shell with full trap, set -e, set -x, arithmetic, here-docs) |

### Builtin feature highlights (v0.2.0)

- **`grep`**: `-m N`, `-o`, `-A/-B/-C N` context lines (plus existing `-E/-F/-v/-l/-i/-n/-c/-r`)
- **`sort`**: `-M` month sort (plus existing `-r/-u/-n/-t/-k/-s`)
- **`xargs`**: `-a FILE`, `-L N`, `-s BYTES` (plus existing `-n/-I/-0/-r/-P`)
- **`head`**: legacy `-N` shorthand (`head -1` = `head -n 1`)
- **`test`**: `-nt`/`-ot`/`-ef` file comparison operators

### Shell compliance

POSIX sh with:
- Full arithmetic: `$((expr))` with bitshift, bitwise, compound assignment (`+=`, `-=`, `<<=`, …)
- `trap` — EXIT, INT, TERM, HUP, QUIT, PIPE, CHLD, USR1, USR2
- `set -e` with all POSIX exemptions (conditions, `&&`/`||`, negation)
- `set -x` tracing (`MATCHBOX_TRACE=1` env var alternative)
- Here-documents with `<<`, `<<-`, quoted delimiter (no expansion)
- All POSIX parameter expansions: `${var:-default}`, `${#var}`, `${var%pat}`, etc.
- Function definitions, local variables, recursive calls (depth limit 1000)

## Architecture

- **Multicall binary**: symlink tool names to `matchbox`; dispatch by `argv[0]`
- **POSIX shell**: full recursive-descent parser in `src/shell/`
- **Module system**: unknown flags trigger `dlopen` of a `.so` module from `/usr/lib/matchbox/modules/`
- **io_uring batching**: independent filesystem operations submitted as a single batch (auto-detected; falls back gracefully; override with `MATCHBOX_FORCE_FALLBACKS=1`)
- **Filesystem cache**: avoids redundant `stat()` calls within a build script
- **Thompson NFA/DFA regex**: fast pattern matching for `grep` and `sed`; BMH literal fast path

## Environment variables

| Variable | Effect |
|----------|--------|
| `MATCHBOX_TRACE=1` | Print each command to stderr before execution (`set -x` style) |
| `MATCHBOX_TRACE=2` | As above, plus `[builtin]` tag for builtin commands |
| `MATCHBOX_FORCE_FALLBACKS=1` | Disable io_uring and inotify; use portable fallback paths |

## Testing

```sh
make test             # unit + C tests
make compat-test      # 54 TAP compat tests vs system tools
make integration-test # end-to-end shell scripts
make security-test    # rm safety and privilege checks
make check            # full quality audit (size, version, all suites)
```

## License

BSD 2-Clause — see `LICENSE`.
