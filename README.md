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

That Dockerfile uses mold, ninja, clang, sccache, and jemalloc. You
didn't configure any of them.

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

Measured on nlohmann/json test suite (186 build steps, 84 executables,
32 cores):

    Ubuntu 24.04, GCC 13, GNU ld       ~50s    baseline
    Wolfi, GCC 15, GNU ld              ~64s    OpenSSF flags add per-file overhead
    Wolfi, Clang 18, GNU ld            ~43s
    Wolfi, Clang 18, mold              ~43s    compile-bound; mold neutral here
    silex:slim, cold sccache           ~44s
    silex:slim, warm sccache           ~2.5s

Cold build improvement is 14-33% depending on workload, mostly from
clang. The significant gain is incremental: ~20x with a warm sccache.

Run `benchmarks/benchmark.sh` to reproduce.

## Building silex

```bash
git clone https://github.com/richarah/silex.git
cd silex
bash scripts/setup-dev.sh   # once, configures git hooks
bash scripts/build.sh       # builds silex:slim
bash scripts/test.sh        # 43 tests
```

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
