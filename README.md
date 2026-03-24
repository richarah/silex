# silex

A Docker image where everything is fast.

Change your FROM line. Keep everything else.

```dockerfile
# before
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y build-essential cmake
# 47 seconds before a single file compiles

# after
FROM silex:slim
# ready
```

Your Dockerfile doesn't change. Your build time does.

## Why this exists

A typical `docker build` on Ubuntu spends 80% of its time not compiling
your code. It downloads packages over a single connection, decompresses
them with single-threaded gzip, runs postinstall scripts that configure
services nobody will start, rebuilds a man page database nobody will
read, and calls fsync() after every file in case the container loses
power mid-apt-get.

Silex removes all of that. What's left is your build, running on tools
that don't have 30 years of backwards compatibility holding them back.
No novel algorithms. Just the right tools, configured correctly.

## What's inside

| Component | Version | Replaces | Why |
|-----------|---------|----------|-----|
| Compiler | clang 18.1.8 | gcc | 14-33% faster on template-heavy C++ (benchmarked) |
| Linker | mold 2.40.4 | ld/gold/lld | 5-10x faster per link step |
| Build system | ninja 1.12.1 | make | Better job scheduling, lower overhead |
| Compiler cache | sccache 0.8.2 | ccache | 21x faster warm rebuilds. Remote backends (S3, GCS, Redis). |
| Allocator | mimalloc 2.1.7 | glibc malloc | 9% faster under multi-threaded builds (benchmarked). LD_PRELOAD. |
| Compression | zstd 1.5.6 | gzip | Parallel. |
| Coreutils | busybox 1.37.0 | GNU coreutils | Single binary. Benchmarked fastest. |
| Shell | dash 0.5.12 | bash | /bin/sh. 4x faster startup. |
| Package mgr | apk (Wolfi) | apt/dpkg | No postinstall scripts. Sub-second installs. |
| PID 1 | tini 0.19.0 | nothing | Signal reaping, zombie prevention |

Also ships fd and ripgrep alongside find and grep.

Base: debian:bookworm-slim (glibc). Every binary compiled from source
with pinned SHA256 tarballs, -O3, LTO. Runtime packages via apk from
Wolfi repos. Not Alpine (musl is slower). Not a Wolfi base image
(building from source gives version control Wolfi's rolling repo can't).

## Image variants

| Tag | What | Size |
|-----|------|------|
| `silex:slim` | Everything above | ~900MB uncompressed, ~360MB pull |
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

clang, mold, Ninja, sccache, mimalloc are pre-configured. Cold builds
are 6-32x faster than ubuntu:24.04. Warm sccache rebuilds are 21x.

## Configuration

Environment variables. Override in your Dockerfile or at runtime.

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

Most Dockerfiles assume Debian. Silex isn't Debian, but it speaks
Debian.

```dockerfile
RUN apt-get install -y cmake libssl-dev libcurl4-openssl-dev
# becomes: apk add cmake openssl-dev curl-dev
# same result. sub-second instead of 30 seconds.
```

504 package mappings. Package installs run with silex-nosync.so
preloaded, suppressing fsync calls that are pointless inside a
container layer.

When the shim doesn't know a package, it tries as-is:

    silex apt-shim: libobscure-dev: no mapping, trying as-is

Disable with `ENV SILEX_WRAPPERS=off`.

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

Most `apk add` commands work unchanged. Binaries compiled against musl
won't run in Silex (glibc). Recompile from source.

### Common gotchas

**GCC hardcoded.** CMakeLists sets `CMAKE_C_COMPILER=gcc`? Either
`apk add gcc` or unset it and let Silex default to clang.

**`/bin/sh` is dash.** `[[ ]]`, arrays, process substitution will
fail. Use `#!/bin/bash` or write POSIX sh.

**`python` not on PATH.** Silex has `python3`. Symlink if needed:
`RUN ln -sf /usr/bin/python3 /usr/bin/python`.

**Package not mapped.** `apk search <name>` to find the Wolfi name.
File an issue to add the mapping.

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
`apt-get update` takes 10-30 seconds; `apk` takes under one. Isolated
compilation: clang is 14-33% faster than gcc on template-heavy C++.

SQLite is a known anti-pattern: 230k-line single translation unit.
clang -O3 does more analysis than gcc on files that large. Set
`CC=gcc`. `silex lint` detects this.

**The number that actually matters:** warm sccache rebuild: 2.4s vs
44s cold. 21x. Change one file, rebuild in 2 seconds. Every commit,
every push, every PR.

Reproduce: `benchmarks/benchmark.sh`.

## What Silex doesn't do

**Fix your Dockerfile.** COPY the whole repo before installing deps
and every source change invalidates the cache. `silex lint` catches
this.

**Replace BuildKit.** Silex is a base image, not a build backend.

**Run in production.** Use `silex:runtime`. Build images ship
compilers. Production images shouldn't.

**Make Python fast.** uv is included and helps with pip. If your
build is slow because you're training a model in the Dockerfile,
that's between you and your choices.

## Diagnostics

    $ docker run silex:slim silex doctor

Configuration, tool versions, allocator, cache status, BuildKit cache
mount paths.

    $ silex lint Dockerfile [source_dir]

Catches: COPY before deps, missing cache mounts, `RUN curl` where
`ADD --checksum` works, translation units over 100k preprocessed
lines where gcc beats clang.

## Troubleshooting

**mold + sccache misses every build.** Known issue
([mozilla/sccache#1755](https://github.com/mozilla/sccache/issues/1755)).
Use lld: `ENV SILEX_LINKER=lld`.

**LD_PRELOAD conflict.** Your app preloads its own library:
`ENV SILEX_MALLOC=system`.

**Locale errors.** Silex sets `LC_ALL=C`. Override:
`ENV LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8`.

**Disable everything:**

```sh
docker run --rm \
  -e SILEX_MALLOC=system -e SILEX_WRAPPERS=off \
  -e SILEX_APT_SHIM=off -e SILEX_CACHE=off \
  silex:slim sh
```

**Reporting bugs.** https://github.com/richarah/silex/issues.
Include `silex doctor` output, the failing Dockerfile, and full
error output.

## Host setup

### BuildKit cache mounts

```dockerfile
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --build build

RUN --mount=type=cache,target=/root/.cargo/registry \
    cargo build --release
```

Paths: sccache `/root/.cache/sccache`, pip `/root/.cache/pip`,
uv `/root/.cache/uv`, cargo `/root/.cargo/registry`, npm `/root/.npm`.

### Layer compression

BuildKit supports zstd layer compression. Smaller and faster than gzip:

```sh
docker build --output type=image,compression=zstd .
```

The ~900MB image pulls as ~360MB with zstd.

### File descriptor limits

Linkers and compilers open many fds. On busy hosts:

```sh
docker run --ulimit nofile=65535:65535 silex:slim ...
```

## Building Silex

```sh
git clone https://github.com/richarah/silex.git
cd silex
make verify-sources    # SHA256 check, once
make bootstrap         # from debian:bookworm-slim, ~90-120 min
make build             # from previous silex:slim, ~15-20 min
make test              # compat tests
```

`make bootstrap` compiles everything from source. All tarballs
verified against `sources.json`.

`make build` uses the previous release's clang and mold, skipping
the 60-minute LLVM step. Silex builds Silex.

## Known limitations

**SQLite and amalgamation builds.** clang -O3 is 10x slower than
gcc on 230k-line translation units. `CC=gcc`. `silex lint` detects
this.

**Wolfi names differ from Debian.** 504 packages mapped.
`apk search` for the rest.

**No GPU in slim.** CPU only.

**Coreutils are busybox.** `sort --parallel` works (GNU sort
wrapper). Other GNU-only flags don't.

**git not in slim.** Use `silex:dev` or `apk add git`.

**Image is ~900MB uncompressed.** libLLVM.so alone is ~400MB
stripped. Pulls as ~360MB. `silex:runtime` is 30MB.

## FAQ

**Why not Alpine?**
musl. Measurably slower for compilation. Alpine is consistently
slower than Ubuntu in our benchmarks despite a newer gcc.

**Why build from source?**
Wolfi is rolling. Chainguard deletes packages after 12 months.
Pinned Dockerfiles break. Source tarballs with SHA256 don't.

**Will this break my Dockerfile?**
Maybe. 504 packages mapped, not all. `SILEX_WRAPPERS=off` for raw
apk. File a bug.

**Is the 31x real?**
Yes, but includes apk-vs-apt install time. Isolated compilation:
14-33%. Warm sccache: 21x. We show all three because we'd rather
be trusted than impressive.

**How about a Rust rewrite?**
It's a Dockerfile.

## Licence

MIT. See `LICENSE`.

Bundled tool licences and source manifest: [docs/LICENSING.md](docs/LICENSING.md).
