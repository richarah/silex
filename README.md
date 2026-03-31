# silex

Shell and coreutils for container builds.

The Unix process model forks a new process for every
command. In a container build invoking 500 commands per
minute, half the wall time is process creation.

silex is one process. Commands run as builtins
inside a persistent shell. No fork, no exec, no load.
Flags the builtins don't handle are loaded from modules
at runtime or forwarded to the external tool in PATH.

## Usage

    COPY --from=silex /bin/silex /bin/silex
    RUN silex --install /usr/local/bin

Or use a pre-built base image:

    FROM silex:alpine

Existing scripts work unchanged.

## What's inside

A POSIX sh. Thirty-two coreutils as builtins. A module
loader. A regex engine. An arena allocator. An
io_uring submission path for batching independent
filesystem operations. A filesystem state cache.

The builtins handle POSIX flags and the GNU flags
that appear in container workloads. Everything else
delegates.

## Performance

Measured on Alpine 3.21, x86_64. Mean of 3 runs.

    wc -l           3-4x faster than GNU wc
    find -name      2x faster than GNU find
    head, tail      3x faster
    sort            1.5x faster
    cp, cat, sed    equal (syscall-bound)

2-3x on real Dockerfile RUN steps. Compilation
time is unchanged; silex replaces the shell
around your compiler, not the compiler.

## Images

    silex:latest     FROM scratch, just the binary (<2MB)
    silex:alpine     Alpine + silex as /bin/sh
    silex:debian     Debian slim + silex as /bin/sh
    silex:ubuntu     Ubuntu + silex as /bin/sh

For the full build SDK with clang, mold, ninja,
sccache, and zstd: see silex-sdk.

## Modules

When the argument parser encounters a flag the
builtin doesn't handle, it scans /usr/lib/silex/modules/
for a .so that claims the flag. Found: dlopen, call,
close. Not found: fork to external tool. Module files
are verified for ownership and permissions before load.

## Building

    make release          # musl, static, ~1MB
    make release-glibc    # glibc, dynamic, ~400KB
    make test

## Environment

    SILEX_MODULE_PATH   extra module directories
    SILEX_FSCACHE_TTL   cache lifetime (seconds, default 5)
    SILEX_PROFILE       print summary on exit
    SILEX_TRACE         trace command dispatch

## Limitations

POSIX sh only. Bash syntax requires modules.
LC_ALL=C always. No interactive use. grep BRE is
within 1.5x of GNU; backreferences fall back to
libc. sort is faster on small inputs, slower on
inputs exceeding memory.

## Requirements

x86_64 (AVX2) or aarch64. Linux 4.5+.
Older kernels work with reduced optimisation.

## See also

silex-sdk — the Docker build image that ships silex.
toybox(1) — the upstream codebase.

## Licence

BSD 2-clause.
