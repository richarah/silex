# silex

A Docker base image where everything is fast.

Change your FROM line. Keep everything else.

```dockerfile
- FROM ubuntu:24.04
+ FROM silex:slim
```

Your Dockerfile doesn't change. Your build time does.
Cold builds 6-32x faster. Warm rebuilds 21x.

## Why this exists

Because `dpkg --configure -a` has mass.

A typical `docker build` on Ubuntu spends most of its time not
compiling your code. Downloading packages over a single connection.
Decompressing them with single-threaded gzip. Running postinstall
scripts that configure services nobody will start. Rebuilding a
man page database nobody will read. Calling fsync() after every
file in case the container loses power mid-apt-get.

Silex removes all of that. What's left is your build, running
on tools that weren't designed around floppy disk throughput.

## What's inside

| Component | Version | Replaces | Why |
|-----------|---------|----------|-----|
| Compiler | clang 18.1.8 | gcc | 14-33% faster on template-heavy C++ |
| Linker | mold 2.40.4 | ld/gold/lld | 5-10x faster per link step |
| Build system | ninja 1.12.1 | make | Better job scheduling, lower overhead |
| Compiler cache | sccache 0.8.2 | ccache | 21x faster warm rebuilds. Remote backends. |
| Allocator | mimalloc 2.1.7 | glibc malloc | 9% faster under multi-threaded builds. LD_PRELOAD. |
| Compression | zstd 1.5.6 | gzip | Parallel. |
| Coreutils | busybox 1.37.0 | GNU coreutils | Single binary. Fastest in benchmark. |
| Shell | dash 0.5.12 | bash | /bin/sh. 4x faster startup. |
| Package mgr | apk (Wolfi) | apt/dpkg | No postinstall scripts. Sub-second installs. |
| PID 1 | tini 0.19.0 | nothing | Signal reaping, zombie prevention |

Also ships fd and ripgrep alongside find and grep.

Base: debian:bookworm-slim (glibc). Every binary compiled from
source with pinned SHA256 tarballs, -O3, LTO. Runtime packages
via apk from Wolfi repos.

Not Alpine (musl is slower for compilation). Not a Wolfi base
image (building from source gives version control that a rolling
repo can't).

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

## Quick start

```dockerfile
FROM silex:slim
RUN apt-get install -y libssl-dev
COPY . /src
WORKDIR /src
RUN cmake -B build && cmake --build build
```

clang, mold, Ninja, sccache, mimalloc are pre-configured.
apt-get works (see apt shim below).

## Configuration

Override in your Dockerfile or at runtime.

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

## apt shim

Silex isn't Debian, but it speaks Debian.

```dockerfile
RUN apt-get install -y cmake libssl-dev libcurl4-openssl-dev
# becomes: apk add cmake openssl-dev curl-dev
# sub-second instead of 30 seconds.
```

504 package mappings. Installs run with silex-nosync.so preloaded,
suppressing fsync calls that are pointless inside a build layer.

When the shim doesn't know a package, it tries as-is:

    silex apt-shim: libobscure-dev: no mapping, trying as-is

Disable: `ENV SILEX_WRAPPERS=off`.

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

build-essential, cmake, ninja-build are preinstalled. Remove them.
`--mount=type=cache` persists sccache across builds.

### From alpine

Most `apk add` commands work unchanged. Binaries compiled against
musl won't run (glibc). Recompile from source.

### Gotchas

**GCC hardcoded in CMakeLists.** `apk add gcc` or unset it.

**`/bin/sh` is dash.** `[[ ]]` and arrays won't work. POSIX sh or
`#!/bin/bash`.

**`python` not on PATH.** `python3` is. Symlink if needed.

**Package not mapped.** `apk search <n>`. File an issue.

## Benchmarks

Full build including package install. Docker 29.2.0, Linux 6.17.0,
x86_64, 32 cores, cold cache, 2 runs averaged.

    project           silex       ubuntu       speedup
    nlohmann/json     1,600ms     50,843ms     31.8x
    fmtlib            1,423ms     20,170ms     14.1x
    googletest        1,172ms      7,017ms      5.9x
    abseil-cpp        1,350ms      7,814ms      5.7x
    google/re2        1,061ms      1,206ms      1.1x      (tie)
    SQLite amalgam   12,728ms      1,268ms      0.1x      SLOWER

The honest version: the large speedups include package install time.
`apt-get update` takes 10-30 seconds; `apk` takes under one.
Isolated compilation: clang is 14-33% faster than gcc on
template-heavy C++.

SQLite is a known anti-pattern: 230k-line single TU. clang -O3
does more work than gcc on files that large. `CC=gcc`.
`silex lint` detects this.

**The number that actually matters:** warm sccache rebuild, 2.4s
vs 44s cold. 21x. Change one file, rebuild in 2 seconds. Every
commit, every push, every PR. That's the value proposition.

Reproduce: `benchmarks/benchmark.sh`.

## What Silex doesn't do

**Fix your Dockerfile.** COPY the whole repo before installing deps
and every source change invalidates the cache. `silex lint` catches
this.

**Replace BuildKit.** Base image, not build backend.

**Run in production.** `silex:runtime`. Build images ship compilers.
Production images shouldn't.

**Make Python fast.** uv is included and helps with pip. If your
build is slow because you're training a model in the Dockerfile,
that's between you and your choices.

## Diagnostics

    $ docker run silex:slim silex doctor

Configuration, versions, allocator, cache status, BuildKit paths.

    $ silex lint Dockerfile [source_dir]

Catches: COPY before deps, missing cache mounts, `RUN curl` where
`ADD --checksum` works, TUs over 100k preprocessed lines where
gcc beats clang.

## Troubleshooting

**mold + sccache cache misses.** Known
([mozilla/sccache#1755](https://github.com/mozilla/sccache/issues/1755)).
`ENV SILEX_LINKER=lld`.

**LD_PRELOAD conflict.** `ENV SILEX_MALLOC=system`.

**Locale errors.** Silex sets `LC_ALL=C`.
`ENV LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8` to override.

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

`make bootstrap` compiles everything from source. Tarballs verified
against `sources.json`.

`make build` reuses the previous release's clang and mold, skipping
the 60-minute LLVM step. Silex builds Silex.

## Known limitations

**SQLite and amalgamation builds.** clang -O3 is 10x slower than
gcc on 230k-line TUs. `CC=gcc`. `silex lint` detects this.

**504 packages mapped, not all.** `apk search` for the rest.

**No GPU in slim.** CPU only.

**Coreutils are busybox.** `sort --parallel` works via GNU sort
wrapper. Other GNU-only flags don't.

**git not in slim.** `silex:dev` or `apk add git`.

**~900MB uncompressed.** libLLVM.so is ~400MB of that.
`silex:runtime` is 30MB.

## FAQ

**Why not Alpine?**
musl. Measurably slower for compilation in our benchmarks, despite
a newer gcc. The overhead is the libc, not the compiler.

**Why build from source?**
Wolfi is rolling. Chainguard deletes packages after 12 months.
Pinned Dockerfiles break. Source tarballs with SHA256 don't.

**Will this break my Dockerfile?**
Maybe. `SILEX_WRAPPERS=off` for raw apk. File a bug.

**Is the 31x real?**
Yes, but includes apk-vs-apt install time. Isolated compilation:
14-33%. Warm sccache: 21x. We show all three because we'd rather
be trusted than impressive.

**How about a Rust rewrite?**
It's a Dockerfile.

## Licence

MIT. See `LICENSE`.

Bundled tool licences and source manifest:
[docs/LICENSING.md](docs/LICENSING.md).
