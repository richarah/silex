# silex

A Docker base image where everything is fast. `FROM silex:slim` and
your builds inherit clang, mold, ninja, sccache, and jemalloc.

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

| Component | Default | Replaces | Why |
|-----------|---------|----------|-----|
| Compiler | clang-18 | gcc | LLVM integration, ThinLTO, cross-compilation from a single binary |
| Linker | mold 2.35.0 | ld/gold/lld | 5-10x faster per link step |
| Build system | ninja | make | Better job scheduling, lower overhead |
| Compiler cache | sccache 0.8.2 | ccache | Remote backends (S3, GCS, Redis), Rust support |
| Allocator | jemalloc 5.3.0 | glibc malloc | Faster under multi-threaded allocation, loaded via LD_PRELOAD |
| PID 1 | tini 0.19.0 | nothing | Signal reaping, zombie prevention |

Base: cgr.dev/chainguard/wolfi-base (glibc, ~14MB, daily CVE rebuilds).
Not Alpine (musl is slower for compilation). Not Debian (dpkg is the
disease, not the cure).

## Image variants

| Tag | What | Size |
|-----|------|------|
| `silex:slim` | Everything above | ~512MB |
| `silex:dev` | Adds development tooling | larger |
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
apk operation. 449 package mappings included.

```dockerfile
RUN apt-get install -y cmake libssl-dev libcurl4-openssl-dev
# becomes: apk add cmake openssl-dev curl-dev
```

When the shim doesn't know a package:

    silex: package 'libobscure-dev' not found in package map.
           Try: apk search obscure

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
bash scripts/setup-dev.sh   # once, configures git hooks
bash scripts/build.sh       # builds silex:slim
bash scripts/test.sh        # 43 tests
```

## Known limitations

**SQLite and other amalgamation builds.** clang's optimiser is more
aggressive than gcc on very large single translation units. On the SQLite
amalgamation (230k preprocessed lines), clang -O2 is 10x slower than gcc.
Set `CC=gcc` for these files. `silex lint` detects this automatically
(v0.2+, requires source directory as second argument).

**Wolfi package names differ from Debian.** The apt-get shim covers 449
common packages. If `apt-get install libfoo-dev` fails with "not found in
package map", run `apk search foo` to find the Wolfi name and file an issue.

**No GPU support in slim.** nvCOMP decompression and GPU-accelerated hashing
are in `silex:full` (not yet released). `silex:slim` is CPU-only.

**Coreutils are Busybox.** Wolfi's base uses Busybox, not GNU coreutils or
uutils. Some GNU-specific flags won't work. The sort wrapper adds
`--parallel=$(nproc)` only if the installed sort supports it; falls back
silently.

**git is not in silex:slim.** It's in `silex:dev`. If your build stage needs
git for FetchContent or submodules, either use `silex:dev` as the build base
or `RUN apk add git` to install it. `silex:slim` has zero GPLv2 components.

## FAQ

**Why Wolfi?**
glibc (faster than musl for compilation), container-native, daily CVE
rebuilds, works with Trivy/Grype/Snyk. Alpine was the alternative but
musl is measurably slower for the one thing this image is supposed to do.

**Will this break my Dockerfile?**
Maybe. The apt shim covers 449 common packages. If something breaks,
`SILEX_WRAPPERS=off` gives you raw apk. File a bug with the package name.

**Can I use gcc instead of clang?**
`ENV SILEX_CC=gcc SILEX_CXX=g++`. gcc is in the image.

**Why is the image 512MB?**
Clang is 150-200MB. The rest is build tools. `silex:runtime` is ~30MB
for when you only need to run the output.

## License

MIT. See LICENSE.

Bundled tools: clang/LLVM (Apache 2.0 + LLVM exceptions), mold (MIT),
sccache (Apache 2.0), ninja (Apache 2.0), jemalloc (BSD 2-Clause),
tini (MIT), Wolfi base (Apache 2.0).
