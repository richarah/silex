# matchbox

A purpose-built container build runtime: a single static binary containing a minimal POSIX shell and common coreutils as builtins, optimised for the container build hot path.

matchbox makes `RUN` steps in Dockerfiles faster by executing common commands in-process — no fork, no exec, no dynamic linking overhead.

## Quick start

```sh
make
./build/bin/matchbox --install /usr/local/bin
```

This installs symlinks so `cp`, `echo`, `mkdir`, etc. all invoke the matchbox binary.

## Build

```sh
make          # release build (static)
make debug    # debug build with ASan/UBSan
make test     # run unit tests
make install PREFIX=/usr/local
```

Requires: C11 compiler, GNU Make. Uses `musl-gcc` if available, otherwise `gcc -static`.

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
