# matchbox

A purpose-built container build runtime: a single static binary containing a minimal POSIX shell and common coreutils as builtins, optimised for the container build hot path.

matchbox makes `RUN` steps in Dockerfiles faster by executing common commands in-process — no fork, no exec, no dynamic linking overhead.

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

**Architecture note:** distributed x86-64 binaries require AVX2 (Intel Haswell 2013+
or AMD Excavator 2015+). For older hardware:
```sh
make release MARCH=x86-64-v2
```

Requires: C11 compiler (gcc or clang), GNU Make, linux-headers.

## Applets (Phase 1)

| Applet | Flags |
|--------|-------|
| `cp`   | `-rRpfivLHPantuT` |
| `echo` | `-neE` |
| `mkdir`| `-pvm MODE` |

## Architecture

- **Multicall binary**: symlink tool names to `matchbox`; dispatch by `argv[0]`
- **Module system**: unknown flags trigger `dlopen` of a `.so` module from `/usr/lib/matchbox/modules/`
- **io_uring batching**: independent filesystem operations submitted as a single batch
- **Filesystem cache**: avoids redundant `stat()` calls within a build script

See `PROMPT.md` for the full specification.

## License

BSD 2-Clause — see `LICENSE`.
