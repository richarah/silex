# silex

A Docker base image where everything is fast. `FROM silex:slim` and
your builds inherit clang, mold, ninja, sccache, and mimalloc.

Replaces `FROM ubuntu:24.04`. Does not replace Docker, BuildKit, or
your build system. Just makes them faster.

## What it's for

CI/CD pipelines, local development, anywhere you're waiting for
`apt-get install build-essential` to finish its 200 postinstall scripts
so you can compile 12 lines of C++.

## Why this exists

Because `dpkg --configure -a` has mass.

Docker builds are slow. Not because Docker is slow, but because
everything inside the image is slow: the package manager is slow, the
linker is slow, the coreutils fork a new process for every invocation.

Silex picks the fastest implementation of each component and wires them
together. No novel algorithms. Just the right tools, configured
correctly.

## Quick start

```dockerfile
FROM silex:slim
RUN apt-get install -y libssl-dev   # works, see apt shim below
COPY . /src
WORKDIR /src
RUN cmake -B build && cmake --build build
```

That's it. Silex is pre-configured with clang, mold (fast linker), Ninja (fast build system), and sccache (compilation cache). Cold builds are 10-30% faster than `ubuntu:24.04 + build-essential`; **incremental builds are 15-20x faster** thanks to sccache.

## What's inside

| Component | Version | Replaces | Why |
|-----------|---------|----------|-----|
| Compiler | clang 18.1.8 | gcc | LLVM integration, ThinLTO, cross-compilation from a single binary |
| Linker | mold 2.40.4 | ld/gold/lld | 5-10x faster per link step |
| Build system | ninja 1.12.1 | make | Better job scheduling, lower overhead |
| Compiler cache | sccache 0.8.2 | ccache | Remote backends (S3, GCS, Redis), Rust support |
| Allocator | mimalloc 2.1.7 | glibc malloc | 9% faster than system allocator under multi-threaded builds, loaded via LD_PRELOAD |
| PID 1 | tini 0.19.0 | nothing | Signal reaping, zombie prevention |

Base: debian:bookworm-slim (glibc). Every binary compiled from source
with pinned SHA256 values. Runtime packages installed via apk from
Wolfi/Chainguard repos.

Not Alpine (musl is slower for compilation). Not a Wolfi base image
(building from source gives reproducible, auditable binaries independent
of upstream rebuild schedules).

## Image variants

| Tag | What | Size |
|-----|------|------|
| `silex:slim` | Everything above | ~540MB |
| `silex:dev` | Adds development tooling (git, etc.) | larger |
| `silex:runtime` | Companion for multi-stage builds, no compiler | ~30MB |
| `silex:cross` | Adds cross-compilation support (arm64 <-> x86_64) | larger |

## Configuration

Environment variables, set in your Dockerfile or at runtime.

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

Example:

```dockerfile
FROM silex:slim
ENV SILEX_LINKER=lld
ENV SILEX_GENERATOR="Unix Makefiles"
RUN cmake -B build && cmake --build build
```

## apt shim

Most Dockerfiles assume Debian. Silex isn't Debian, but it pretends to
be. An `apt-get` shim translates `apt-get install` into the equivalent
apk operation. 504 package mappings included.

```dockerfile
RUN apt-get install -y cmake libssl-dev libcurl4-openssl-dev
# becomes: apk add cmake openssl-dev curl-dev
```

Package installs run with `silex-nosync.so` preloaded, suppressing `fsync`/`fdatasync` calls that are redundant inside container build layers. Saves ~20% of `apk add` time.

When the shim doesn't know a package:

    silex apt-shim: libobscure-dev: no mapping, trying as-is

Disable with `ENV SILEX_WRAPPERS=off`.

## Diagnostics

```
$ docker run silex:slim silex doctor
```

Prints current configuration, tool versions, and suggested BuildKit
cache mount paths.

## Benchmarks

Full build times including package install (apk vs apt). Docker 29.2.0,
Linux 6.17.0, x86_64, 32 cores, cold cache, 2 runs averaged.

    Project           silex       ubuntu      speedup    alpine      speedup
    nlohmann/json     1,600ms     50,843ms    31.8x      74,438ms    46.1x
    fmtlib            1,423ms     20,170ms    14.1x      —
    googletest        1,172ms      7,017ms     5.9x      —
    abseil-cpp        1,350ms      7,814ms     5.7x      —
    google/re2        1,061ms      1,206ms     1.1x      —           (tie)
    SQLite amalgam   12,728ms      1,268ms     0.1x      —           SLOWER

The large speedups include apk install time. apt-get update alone takes
10-30 seconds; apk takes under a second. Isolated compilation: clang is
14-33% faster than GCC for template-heavy C++.

SQLite is a known anti-pattern: 230k-line single translation unit, clang
-O2. GCC is faster here. If you build SQLite, set CC=gcc.

Incremental build with warm sccache: 2.4s vs 44s cold (21x). This is
the core value proposition for CI pipelines and inner-loop development.

Run `benchmarks/benchmark.sh` to reproduce.

## Building silex

```bash
git clone https://github.com/richarah/silex.git
cd silex

# Verify source SHA256 values (required once before first build)
make verify-sources

# Cold-start build from debian:bookworm-slim (~90-120 min)
make bootstrap

# Self-hosted build using previous silex:slim (~15-20 min)
make build

# Run compat tests
make test
```

`make bootstrap` compiles everything from source: LLVM 18.1.8, mold,
ninja, zstd, mimalloc, busybox, dash, tini, pigz, pixz, GNU coreutils
(sort only), apk-tools, sccache, fd, ripgrep, uv. All tarballs verified
against SHA256 values in `sources.json`.

`make build` reuses the clang and mold from the previous `silex:slim`,
skipping the ~60 minute LLVM compilation step.

## Known limitations

**SQLite and other amalgamation builds.** clang's optimiser is more
aggressive than gcc on very large single translation units. On the SQLite
amalgamation (230k preprocessed lines), clang -O2 is 10x slower than gcc.
Set `CC=gcc` for these files. `silex lint` detects this automatically
(v0.2+, requires source directory as second argument).

**Wolfi package names differ from Debian.** The apt-get shim covers 504
common packages. If `apt-get install libfoo-dev` fails, run
`apk search foo` to find the Wolfi name and file an issue.

**No GPU support in slim.** `silex:slim` is CPU-only.

**Coreutils are Busybox.** Some GNU-specific flags won't work. The sort
wrapper adds `--parallel=$(nproc)` via a GNU coreutils sort binary
installed alongside busybox; other GNU sort flags are not available.

**git is not in silex:slim.** It's in `silex:dev`. If your build stage needs
git for FetchContent or submodules, either use `silex:dev` as the build base
or `RUN apk add git` to install it. `silex:slim` has zero GPLv2 components.

## FAQ

**Why debian:bookworm-slim as base?**
glibc (faster than musl), container-native, daily CVE rebuilds for the
Wolfi package layer. Building from source rather than using a Wolfi base
image gives a reproducible, auditable image where every binary can be
traced back to a specific commit and SHA256-verified tarball.

**Why Wolfi repos for runtime packages?**
glibc-compatible, container-native, daily CVE rebuilds, works with
Trivy/Grype/Snyk. Alpine repos use musl ABIs and are not compatible.

**Will this break my Dockerfile?**
Maybe. The apt shim covers 504 common packages. If something breaks,
`SILEX_WRAPPERS=off` gives you raw apk. File a bug with the package name.

**Can I use gcc instead of clang?**
`ENV SILEX_CC=gcc SILEX_CXX=g++`. Install gcc with `apk add gcc`.

**Why is the image ~540MB?**
Clang is 150-200MB. The rest is build tools. `silex:runtime` is ~30MB
for when you only need to run the output.

## License

MIT. See LICENSE.

Bundled tools (all compiled from source): clang/LLVM 18.1.8 (Apache 2.0
+ LLVM exceptions), mold 2.40.4 (MIT), sccache 0.8.2 (Apache 2.0),
ninja 1.12.1 (Apache 2.0), mimalloc 2.1.7 (MIT), tini 0.19.0 (MIT),
busybox 1.37.0 (GPL-2.0), dash 0.5.12 (BSD), pigz 2.8 (zlib),
pixz 1.0.7 (BSD), zstd 1.5.6 (BSD/GPL-2.0), xxhash 0.8.2 (BSD),
apk-tools 2.14.4 (GPL-2.0), GNU coreutils 9.5 sort (GPL-3.0),
fd 10.2.0 (MIT/Apache-2.0), ripgrep 14.1.1 (MIT/Unlicense),
uv 0.4.30 (MIT/Apache-2.0).
