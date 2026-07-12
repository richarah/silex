# silex

Shell and coreutils for container builds.

`./configure` and `make` spawn a fresh `/bin/sh` for every
test and every recipe line — thousands of them. What
dominates is not how fast the shell *runs*, but how fast it
*starts*.

silex is a small, statically linked, multi-call binary. It
starts in 371 µs; dash takes 577 µs. Commands run as
builtins in-process, so there is no second exec to pay for
either. Flags the builtins don't handle are loaded from
modules at runtime or forwarded to the external tool in PATH.

As `/bin/sh`, on the same image and workload:

    ./configure (zlib)         240 ms  vs dash  284 ms
    ./configure + make -j4     835 ms  vs dash 1496 ms

Be clear about where that comes from. Roughly two thirds of
the advantage over dash is *static linking*, which any shell
could have — busybox sh starts in 446 µs. The remaining third
is silex being leaner than busybox.

And silex is **3.7x slower than dash at interpreting shell**.
It wins on builds because builds are startup-bound, not
interpretation-bound. If that ever stops being true, silex
loses.

The previous version of this README claimed that "in a
container build invoking 500 commands per minute, half the
wall time is process creation". At the ~500 µs per fork this
project's own docs quote, that is 0.25 s per minute — 0.4%,
not half. The fork thesis was wrong by two orders of
magnitude, and the io_uring and fs-cache subsystems built to
serve it turned out to be dead code with zero callers. See
[docs/WHERE_THE_TIME_GOES.md](docs/WHERE_THE_TIME_GOES.md).

## Usage

    COPY --from=silex /bin/silex /bin/silex
    RUN silex --install /usr/local/bin

Or use a pre-built base image:

    FROM silex:alpine

Existing scripts work unchanged.

## What's inside

A POSIX sh. Thirty-two coreutils as builtins. A module
loader. A regex engine. An arena allocator.

There is also a filesystem state cache, but it is **off by
default** and you should leave it that way. It is a
wall-clock TTL over `stat()` results, so a cached answer is
served for up to N seconds regardless of what happened to the
filesystem — and its invalidation is a whitelist with exactly
one entry (`mkdir`). `rm`, `mv`, `ln`, `sed -i` and shell
redirections do not invalidate it. Enabling it trades a
correctness hazard for roughly 10µs across a whole Dockerfile.
`SILEX_FSCACHE_TTL` opts in.

The builtins handle POSIX flags and the GNU flags
that appear in container workloads. Everything else
delegates.

There used to be an "io_uring submission path for batching
independent filesystem operations" listed here. It had zero
callers — `rm` called plain `unlink()` — so it was never on
any code path. It has been removed. It also carried a
data-loss bug (the submission ring was capped at 256 entries
while the fill loop was unbounded, so beyond 256 files
entries were overwritten: some files skipped, others unlinked
twice) and enabled `IORING_SETUP_SQPOLL` whenever running as
root — which, in a container build, is always — spawning a
kernel thread per ring, set up and torn down on every call.

Measured, the upside was not there either: io_uring saves
syscall *entry* overhead, not the VFS work, so batching 1000
unlinks saves on the order of 1ms. See git history if you
want it back.

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

## Testing

silex includes comprehensive test suites for validation:

    make test              # Unit tests (C + shell)
    make compat-test       # Compatibility tests vs GNU tools
    make integration-test  # Integration tests
    make security-test     # Security validation
    make edge-test         # Edge case handling

### External Test Suites

silex integrates 10 battle-tested external test suites for
conformance validation:

    make external-fetch    # One-time setup (~500MB, 5-10 min)
    make external-test     # Run all external suites

Suites included:

- **Oils/OSH** (~1500 POSIX shell spec tests)
- **Smoosh** (157 tests from formal Coq semantics)
- **modernish** (shell bug catalogue, FTL count must be 0)
- **mksh** (MirBSD Korn Shell regression tests)
- **GNU coreutils** (645 tests - the canonical test suite)
- **GNU grep** (200+ pattern matching tests)
- **GNU sed** (100+ stream editing tests)
- **toybox** (upstream tests)
- **ShellSpec** (BDD framework meta-test)
- **Autoconf** (real-world configure scripts: curl, CPython, OpenSSL, SQLite, zlib)

Critical requirements enforced in CI:
- modernish fatal bug (FTL) count must be 0
- All Autoconf configure scripts must pass (100%)

Individual suites can be run:

    tests/external/run-oils-spec.sh
    tests/external/run-modernish.sh
    tests/external/run-configure.sh
    # ... etc

Results are saved to `tests/external/results/` for triage:

    tests/external/triage.sh results/oils-spec-TIMESTAMP.txt

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
