# silex

A considered Docker base image.

```dockerfile
- FROM ubuntu:24.04
+ FROM silex:slim
```

Cold builds 2-3x faster. Warm sccache rebuilds 18x.
Everything else in your Dockerfile stays the same.
`apt-get` still works. If something doesn't,
`ENV SILEX_WRAPPERS=off` and you're on raw apk.

## Why this exists

Because `dpkg --configure -a` has mass.

The tools inside a typical base image were designed for
systems that persist. A build layer doesn't, it succeeds or it fails.
But dpkg still calls fsync() after
every file it unpacks, because the power might go out
between installing libssl-dev and libcurl-dev. Postinstall
scripts still start services that nothing will ever connect
to. apt still downloads the package index fresh on every
build because it assumes something changed since last time.

Silex replaces them with tools that weren't designed
around floppy disk throughput.

## What's inside

| Component | Version | Replaces | Why |
|-----------|---------|----------|-----|
| Compiler | clang 18.1.8 | gcc | 14-33% faster on template-heavy C++ |
| Linker | mold 2.40.4 | ld/gold/lld | 5-10x faster per link step |
| Build system | ninja 1.12.1 | make | Better job scheduling, lower overhead |
| Compiler cache | sccache 0.8.2 | ccache | 18x faster warm rebuilds. Remote backends. |
| Allocator | mimalloc 2.1.7 | glibc malloc | 9% faster under multi-threaded builds. LD_PRELOAD. |
| Compression | zstd 1.5.6 | gzip | Parallel. |
| Coreutils | busybox 1.37.0 | GNU coreutils | Single binary. Fastest in benchmark. |
| Shell | dash 0.5.12 | bash | /bin/sh. 4x faster startup. |
| Package mgr | apk (Wolfi) | apt/dpkg | No postinstall scripts. Sub-second installs. |
| PID 1 | tini 0.19.0 | nothing | Signal reaping, zombie prevention |

Also ships fd and ripgrep alongside find and grep.

Base: debian:bookworm-slim (glibc). Every binary compiled
from source with pinned SHA256 tarballs, -O3, LTO. Runtime
packages via apk from Wolfi repos.

Not Alpine (musl is slower for compilation). Not a Wolfi
base image (building from source gives version control that
a rolling repo can't).

## Image variants

| Tag | What | Size |
|-----|------|------|
| `silex:slim` | Everything above | ~900MB (~360MB compressed) |
| `silex:dev` | Adds git, ssh, headers, python3 | larger |
| `silex:runtime` | Multi-stage companion. No compiler. | ~30MB |
| `silex:cross` | Cross-compilation (arm64 / x86_64) | larger |

```dockerfile
FROM silex:slim AS build
COPY . /src
WORKDIR /src
RUN cmake -B build && cmake --build build && strip build/myapp

FROM silex:runtime
COPY --from=build /src/build/myapp /usr/local/bin/myapp
CMD ["myapp"]
```

Build stage: fast. Final image: 30MB.

## Configuration

Override in Dockerfile or at runtime.

| Variable | Default | Options |
|----------|---------|---------|
| `SILEX_CC` | clang | any compiler on PATH |
| `SILEX_CXX` | clang++ | any compiler on PATH |
| `SILEX_LINKER` | mold | mold, lld, gold, ld |
| `SILEX_GENERATOR` | Ninja | Ninja, "Unix Makefiles" |
| `SILEX_PARALLEL` | auto | auto, off, or a number |
| `SILEX_CACHE` | sccache | sccache, ccache, off |
| `SILEX_MALLOC` | mimalloc | mimalloc, jemalloc, system |
| `SILEX_WRAPPERS` | on | on, off |
| `SILEX_QUIET` | off | on, off |

## Debian compatibility

Silex isn't Debian, but it's fluent.

### apt shim

```dockerfile
RUN apt-get install -y cmake libssl-dev libcurl4-openssl-dev
# becomes: apk add cmake openssl-dev curl-dev
# sub-second instead of 30 seconds.
```

504 package mappings. Installs run with silex-nosync.so
preloaded, suppressing fsync calls that serve no purpose
inside a build layer.

When the shim doesn't know a package, it tries as-is:

    silex apt-shim: libobscure-dev: no mapping, trying as-is

Disable: `ENV SILEX_WRAPPERS=off`.

### Also shimmed

`apt`, `aptitude`, `apt-cache` (search, show, policy),
`apt-mark` (hold/unhold accepted, no-op), `dpkg` (-l, -s,
-L, --configure -a, --get-selections), `adduser`, `addgroup`,
`dpkg-architecture`, `lsb_release`, `update-alternatives`.

`/etc/os-release` reports `ID_LIKE=debian`.

## Tools

    silex doctor

Configuration, tool versions, allocator status, cache backend,
recommended BuildKit cache mount paths. Run this first when
something doesn't behave.

    silex lint <Dockerfile> [source_dir]

Static analysis. Catches COPY-before-deps (cache invalidation),
missing BuildKit cache mounts, `RUN curl` where `ADD --checksum`
is safer, and translation units over 100k preprocessed lines
where gcc outperforms clang. Pass source_dir for TU analysis.

    silex versions

Every bundled tool, its version, source URL, and SHA256.

## Migration

### From ubuntu:24.04

```dockerfile
# before
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y build-essential cmake ninja-build libssl-dev
COPY . /src
WORKDIR /src
RUN cmake -B build && cmake --build build

# after
FROM silex:slim
RUN apt-get install -y libssl-dev
COPY . /src
WORKDIR /src
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake -B build && cmake --build build
```

build-essential, cmake, ninja-build are preinstalled. Remove
them. `--mount=type=cache` persists sccache across builds.

### From alpine

Most `apk add` commands work unchanged. Binaries compiled
against musl won't run (glibc). Recompile from source.

## Benchmarks

Toolchain install + library install + compile. silex has compilers preinstalled; ubuntu:24.04 downloads build-essential, cmake, and ninja-build from scratch each build. Docker 29.2.0, Linux 6.17.0, x86_64, 32 cores. 4 runs per measurement, highest dropped, 3-run average.

    project           silex       ubuntu       speedup
    nlohmann/json     5,059ms     12,474ms     2.5x
    fmtlib            3,969ms     10,790ms     2.7x
    googletest        4,964ms     10,508ms     2.1x
    abseil-cpp        5,508ms     14,610ms     2.7x
    google/re2        8,991ms     12,073ms     1.3x
    SQLite amalgam   17,749ms     11,366ms     0.6x      SLOWER

The cold speedup is 2-3x. No single component dominates:
clang over gcc, mold over ld, ninja over make, apk over apt.
They compound.

SQLite is a known anti-pattern: 230k-line single translation
unit, clang -O3 does more analysis than gcc on files that
large. `CC=gcc`. (`silex lint` detects this.)

Warm sccache rebuild: 2.4s vs 44s cold. 18x.

Reproduce: `benchmarks/benchmark.sh`.

## What Silex doesn't do

**Fix your Dockerfile.** COPY the whole repo before installing
deps and every source change invalidates the cache.

**Replace BuildKit.** Base image, not build backend.

**Run in production.** `silex:runtime`. Build images ship
compilers. Production images shouldn't.

**Make Python fast.** uv is included and helps with pip. If
your build is slow because you're training a model in the
Dockerfile, that's an entirely different problem.

## Known issues

**SQLite and amalgamation builds.** clang -O3 is slower than
gcc on very large TUs. `CC=gcc`.

**GCC hardcoded in CMakeLists.** `apk add gcc` or remove
the override.

**`/bin/sh` is dash.** `[[ ]]`, arrays, process substitution
are bash. Use POSIX sh or `#!/bin/bash`.

**`python3` on PATH. `python` is not.** Per PEP 394.

**504 common pkgs mapped, not all.** There are 68,000 in Debian.
`apk search` for the rest, or file an issue.

**No GPU in slim.** CPU only.

**Coreutils are busybox.** `sort --parallel` works via GNU
sort wrapper. Other GNU-only flags don't.

**git not in slim.** `silex:dev` or `apk add git`.

**~900MB uncompressed.** libLLVM.so is ~400MB of that.
`silex:runtime` is 30MB.

## Troubleshooting

**mold + sccache cache misses.** Known
([mozilla/sccache#1755](https://github.com/mozilla/sccache/issues/1755)).
`ENV SILEX_LINKER=lld`.

**LD_PRELOAD conflict.** `ENV SILEX_MALLOC=system`.

**Locale errors.** Silex sets `LC_ALL=C`.
`ENV LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8`.

**Disable everything:**

```sh
docker run --rm -e SILEX_MALLOC=system \
  -e SILEX_WRAPPERS=off -e SILEX_CACHE=off silex:slim sh
```

**Bugs:** https://github.com/richarah/silex/issues.
Include `silex doctor` output and the failing Dockerfile.

## Host setup

### BuildKit cache mounts

```dockerfile
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --build build
```

Paths: sccache `/root/.cache/sccache`, pip `/root/.cache/pip`,
uv `/root/.cache/uv`, cargo `/root/.cargo/registry`,
npm `/root/.npm`.

### Layer compression

```sh
docker build --output type=image,compression=zstd .
```

~900MB pulls as ~360MB with zstd.

### File descriptors

```sh
docker run --ulimit nofile=65535:65535 silex:slim ...
```

## Building Silex

```sh
git clone https://github.com/richarah/silex.git
cd silex
make verify-sources    # SHA256, once
make bootstrap         # from debian:bookworm-slim, ~90-120 min
make build             # from previous silex:slim, ~15-20 min
make test              # compat tests
```

`make bootstrap` compiles everything from source. Tarballs
verified against `sources.json`.

`make build` reuses the previous release's clang and mold,
skipping the 60-minute LLVM step. Silex builds Silex.

## FAQ

**Why not Alpine?**
musl. Measurably slower for compilation in our benchmarks,
despite a newer gcc. The overhead is the libc, not the
compiler.

**Why build from source?**
Wolfi is rolling. Chainguard deletes packages after 12 months.
Pinned Dockerfiles break. Source tarballs with SHA256 don't.

**Will this break my Dockerfile?**
Maybe. `SILEX_WRAPPERS=off` for raw apk. File a bug.

**Are the speedups real?**
2-3x cold, compilers preinstalled vs `apt-get install` each
build. 18x warm sccache. SQLite is slower. Methodology and
harness in `benchmarks/`.

**Rust rewrite?**
It's a Dockerfile.

**silex?**
Latin for flint.

## Licence

MIT.

Bundled tool licences and source manifest:
[docs/LICENSING.md](docs/LICENSING.md).
