# silex

A considered Docker base image.

```diff
- FROM ubuntu:24.04
+ FROM silex:slim
```

Cold builds 2-3x faster. Warm sccache rebuilds 18x.

Your Dockerfile doesn't change:

```dockerfile
# this still works
RUN apt-get install -y libssl-dev libcurl4-openssl-dev
COPY . /src && cd /src
RUN cmake -B build && cmake --build build
```

`apt-get install` still works, same packages, same
names. cmake picks up ninja automatically. The linker,
the allocator, the compiler cache are all invisible.
You change one line and the build gets faster.

Every default tool has a faster replacement that
distros can't ship. Here's what Silex ships instead:

| Was | Is | Effect |
|-----|-----|--------|
| gcc | clang 18.1.8 | 14-33% on template-heavy C++ |
| ld / gold / lld | mold 2.40.4 | 5-10x per link step |
| make | ninja 1.12.1 | Better scheduling, lower overhead |
| ccache | sccache 0.8.2 | 18x warm rebuilds. Remote backends. |
| glibc malloc | mimalloc 2.1.7 | 9% under threaded builds. LD_PRELOAD. |
| gzip | zstd 1.5.6 | Parallel |
| GNU coreutils | busybox 1.37.0 | Single binary. Fastest in benchmark. |
| bash | dash 0.5.12 | /bin/sh. 4x startup. |
| apt / dpkg | apt (fast) | No postinstall scripts. Sub-second. |
| (nothing) | tini 0.19.0 | Signal reaping, zombie prevention |

Also ships fd and ripgrep alongside find and grep.
gcc, make, and bash are one `apk add` away. Nothing
is locked in.

Base: debian:bookworm-slim (glibc). Every binary
compiled from source with pinned SHA256 tarballs,
-O3, LTO. Not Alpine (musl is slower for compilation).
Not Wolfi (rolling, packages deleted after 12 months).

---

Everything below is reference.

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

Silex isn't Debian, but it speaks Debian. Without the
postinstall scripts, font cache rebuilds, and fsync
after every file.

### How it works

Under the hood, packages are served by apk, which is
why they install in under a second instead of thirty.
The packages themselves use Debian names. `apt-get
install libssl-dev` installs a package called libssl-dev.
No translation, no mapping.

```dockerfile
RUN apt-get install -y cmake libssl-dev libcurl4-openssl-dev
# sub-second instead of 30 seconds.
```

Installs run with silex-nosync.so preloaded, suppressing
fsync calls that serve no purpose inside a build layer.

`ENV SILEX_WRAPPERS=off` disables the apt compatibility
layer and gives you raw apk.

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

build-essential, cmake, ninja-build are preinstalled.
Remove them. `--mount=type=cache` persists sccache
across builds.

### From alpine

Most `apk add` commands work unchanged. Binaries compiled
against musl won't run (glibc). Recompile from source.

## Benchmarks

Toolchain install + library install + compile. silex has
compilers preinstalled; ubuntu:24.04 downloads build-essential,
cmake, and ninja-build from scratch each build. Docker 29.2.0,
Linux 6.17.0, x86_64, 32 cores. 4 runs per measurement,
highest dropped, 3-run average.

    project           silex       ubuntu       speedup
    nlohmann/json     4,144ms     11,944ms     2.9x
    fmtlib            3,488ms     11,178ms     3.2x
    googletest        4,015ms     11,442ms     2.8x
    abseil-cpp        4,408ms     10,845ms     2.5x
    google/re2        7,562ms     12,354ms     1.6x
    eigen             4,691ms     14,418ms     3.1x
    Boost.Spirit X3  15,672ms     15,757ms     1.0x
    SQLite amalgam    9,928ms     13,670ms     1.4x

The cold speedup is 2-3x. No single component dominates:
clang over gcc, mold over ld, ninja over make, apk over
apt. They compound.

Boost.Spirit X3 is header-only: one file, no linking,
pure template instantiation. That's compiler frontend
work. Silex replaces everything around the compiler,
not the compiler itself. This is the ceiling.

Warm sccache rebuild: 2.4s vs 44s cold. 18x.

Reproduce: `benchmarks/benchmark.sh`.

## What Silex doesn't do

**Fix your Dockerfile.** COPY the whole repo before
installing deps and every source change invalidates
the cache.

**Replace BuildKit.** Base image, not build backend.

**Run in production.** `silex:runtime`. Build images
ship compilers. Production images shouldn't.

**Make Python fast.** uv is included and helps with pip.
If your build is slow because you're training a model
in the Dockerfile, that's between you and your choices.

## Known issues

**SQLite and amalgamation builds.** clang -O3 is slower
than gcc on very large TUs. `CC=gcc`.

**GCC hardcoded in CMakeLists.** `apk add gcc` or
remove the override.

**`/bin/sh` is dash.** `[[ ]]`, arrays, process
substitution are bash. Use POSIX sh or `#!/bin/bash`.

**`python3` on PATH. `python` is not.** Per PEP 394.

**No GPU in slim.** CPU only.

**Coreutils are busybox.** `sort --parallel` works via
GNU sort wrapper. Other GNU-only flags don't.

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

`make bootstrap` compiles everything from source.
Tarballs verified against `sources.json`.

`make build` reuses the previous release's clang and
mold, skipping the 60-minute LLVM step. Silex builds
Silex.

## FAQ

**Why hasn't this been done?**
gcc, ld, make, gzip, dpkg. Nobody chose them. They
accreted: gcc because Debian, ld because it came with
gcc, make because POSIX, gzip because that's what there
was in 1993. Distros can't ship their replacements
because a distro also has to run on hospital MRI
machines, railway signalling systems, and nuclear
submarines. The images that do ship them want you to
rewrite your Dockerfile and learn a new ecosystem.
Your build server is not a submarine, and your sprint
ends Friday.

**What's wrong with dpkg?**
Nothing, if you're running a server for ten years.
In a container that lives for ten seconds, dpkg
rebuilds the font cache for a screen that doesn't
exist, calls fsync() to protect writes that will be
thrown away, and assembles a boot image for a
filesystem that was never meant to boot.

**Why not Alpine?**
musl. Measurably slower for compilation in our
benchmarks, despite a newer gcc. The overhead is
the libc, not the compiler.

**Why not Wolfi?**
Wolfi is rolling. Chainguard deletes packages after
12 months. Pinned Dockerfiles break. Source tarballs
with SHA256 don't.

**Will this break my Dockerfile?**
Possibly. `SILEX_WRAPPERS=off` for raw apk. File a
bug if the issue persists.

**Are the speedups real?**
2-3x cold, compilers preinstalled vs `apt-get install`
each build. 18x warm sccache. Methodology and harness
in `benchmarks/`.

**silex?**
Latin for flint.

## Licence

MIT.

Bundled tool licences and source manifest:
[docs/LICENSING.md](docs/LICENSING.md).
