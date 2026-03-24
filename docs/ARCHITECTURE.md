# Silex Architecture

A technical description of how silex:slim is built and why each decision was made.

---

## 1. Build Pipeline

### Three Stages

The bootstrap build (`dockerfiles/Dockerfile.bootstrap`) has three stages:

```
build-c      FROM debian:bookworm-slim
             GCC builds mold; mold builds stage-1 clang.
             stage-1 clang builds: ninja, zstd, xxhash, mimalloc,
             busybox, dash, tini, pigz, pixz, coreutils sort,
             apk-tools, silex-nosync.so.

build-rust   FROM debian:bookworm-slim (parallel with build-c)
             Rust 1.82.0 builds: sccache, fd, ripgrep, uv.

final        FROM debian:bookworm-slim
             Installs ca-certificates, libssl3, zlib1g via apt.
             Removes apt and dpkg.
             COPYs compiled artifacts from build-c and build-rust.
             Configures apk for Wolfi/Chainguard repos.
             Installs scripts, wrappers, shims.
```

The self-hosted build (`dockerfiles/Dockerfile`) carries `/opt/llvm/` and
`/opt/mold/` forward from the previous `silex:slim` via `COPY --from`,
skipping the ~60-minute LLVM compilation step.

### Why Compile From Source

- Pinned SHA256 on every source tarball. Every binary in the image is
  traceable to a specific commit.
- No dependency on upstream rebuild schedules. The image does not break
  when a Wolfi package is updated.
- Compiler flags chosen for the build image workload: `-O3`, mold as linker,
  `x86-64-v3` microarchitecture target (or `armv8.2-a+crypto` on arm64).

### Base: debian:bookworm-slim

- glibc 2.36. Binaries compiled here run on any glibc >= 2.36 system.
- Minimal attack surface: ~30 packages in the slim variant.
- apt and dpkg are removed after the package install step; apk replaces them.

### Why Not Wolfi Base

Wolfi's package repo (apk.cgr.dev/chainguard) is used for runtime packages
(`apk add` inside user Dockerfiles). The *base image* is Debian because:

- Debian's glibc is the de-facto standard ABI for pre-built binaries.
- Building from source requires a stable, widely available compiler (gcc).
  Debian bookworm provides gcc 12.2, which is sufficient for LLVM 18.
- Wolfi rebuilds daily; a Wolfi-based bootstrap would produce different
  toolchain binaries on different days.

---

## 2. Compiler Flags

### Stage-1 clang (gcc-compiled)

```
-O3
-fuse-ld=/opt/mold/bin/mold
```

No LTO: gcc cannot produce ThinLTO bitcode for LLVM's linker. The flag
`LLVM_ENABLE_LTO=OFF` is set explicitly. `-O3` (rather than `-O2`) makes the
clang binary in the final image faster; there is no correctness risk on a
well-tested codebase like LLVM.

### All other C/C++ tools (compiled by stage-1 clang)

```
-O3 -fomit-frame-pointer -march=x86-64-v3 (or -march=armv8.2-a+crypto)
-fuse-ld=/opt/mold/bin/mold
```

`x86-64-v3` requires AVX2 (Intel Haswell 2013+, AMD Zen 1 2017+). All
modern CI runners meet this bar. Override with `--build-arg MARCH_FLAG=-march=x86-64`
if you need to build on older hardware.

### Rust tools (sccache, fd, ripgrep, uv)

Compiled with the stage-1 clang as linker, `CARGO_PROFILE_RELEASE_LTO=thin`,
`CARGO_PROFILE_RELEASE_OPT_LEVEL=3`. Produces smaller and faster binaries
than the default Rust release profile.

---

## 3. LLVM Strip Step

After installation, `/opt/llvm/` is pruned to only what is needed at
runtime for compilation:

| Kept | Dropped |
|------|---------|
| `clang`, `clang++`, `clang-18` | All other binaries (100+ llvm-* tools) |
| `lld`, `ld.lld` | libclang-cpp.so (~400MB) |
| `libLLVM.so` | libclang.so (C API) |
| `libc++.so`, `libc++abi.so` | libLTO.so, libRemarks.so |
| `compiler-rt` clang_rt libs | All `.a` static libs |
| | `lib/cmake/`, `share/` |

`libclang-cpp.so` is NOT needed at runtime because `CLANG_LINK_CLANG_DYLIB`
was not set at configure time. The clang binary statically links its clang
component code and only dynamically links `libLLVM.so`.

---

## 4. Tool Selection

### Compiler: clang 18.1.8

- Single binary targets both X86 and AArch64 (built with
  `-DLLVM_TARGETS_TO_BUILD="X86;AArch64"` when building for both arches).
- ThinLTO and PGO are better integrated in clang than gcc.
- Faster on template-heavy C++: 14-33% over gcc in measurements.

gcc is not excluded — users can `apk add gcc` and set `CC=gcc`.

### Linker: mold 2.40.4

| Linker | Relative speed | Notes |
|--------|---------------|-------|
| GNU ld | 1x | Baseline |
| gold | 2-5x | Better than ld |
| lld | 2-4x | Good; better sccache compatibility |
| mold | 5-10x | Fastest; parallel by design |

mold is compiled from source. Release tarballs were discontinued around
v2.36; the archive URL is used (`refs/tags/vVERSION.tar.gz`).

### Build System: Ninja 1.12.1

`CMAKE_GENERATOR=Ninja` is set so all CMake projects use Ninja automatically.
Lower overhead than Make; better parallel job scheduling.

### Compiler Cache: sccache 0.8.2

- Remote backends (S3, GCS, Redis, GitHub Actions cache).
- Works with Rust (`RUSTC_WRAPPER`).
- Stateless: no daemon required.
- `SCCACHE_IDLE_TIMEOUT=0` keeps the daemon alive for the container lifetime.

### Allocator: mimalloc 2.1.7

Loaded via `LD_PRELOAD` by the entrypoint. Benchmark on nlohmann/json full
test suite (32 cores, 3 runs):

| allocator | avg time |
|-----------|----------|
| system malloc | 36028ms |
| jemalloc | 34782ms |
| **mimalloc** | **32513ms** |

mimalloc -9% vs system, jemalloc -3% vs system. Both allocators are in the
image; switch with `SILEX_MALLOC=jemalloc` or `SILEX_MALLOC=system`.

### Package Manager: apk-tools 2.14.4

Compiled from source. Points at `https://apk.cgr.dev/chainguard` (Wolfi).
The signing key is copied from `cgr.dev/chainguard/wolfi-base:latest` at
build time. Provides package installs with:

- ~1s `apk update` vs 10-30s `apt-get update`
- No postinstall scripts
- `silex-nosync.so` preloaded during `apk add` to suppress redundant fsync
  calls (saves ~20% of install time)

### Shell: dash 0.5.12 + busybox 1.37.0

`/bin/sh` → dash. Busybox provides all other shell utilities. busybox was
measured as fastest on the benchmark workload (34915ms avg vs GNU 36490ms,
uutils 36239ms).

The `sort` wrapper in `/usr/local/silex/bin/sort` calls a separate
`sort-parallel` binary (GNU coreutils 9.5 sort, compiled from source) which
supports `--parallel=$(nproc)`. This is not exposed system-wide.

---

## 5. Environment Configuration

### SILEX_* Variable Flow

1. `tini` starts, reaps signals, invokes `silex-entrypoint`
2. `silex-entrypoint` reads `SILEX_*` vars, sets `CC`, `CXX`, `LDFLAGS`,
   `CMAKE_GENERATOR`, `MAKEFLAGS`, `RUSTC_WRAPPER`, `LD_PRELOAD`
3. Prints one-line config summary to stderr (unless `SILEX_QUIET=on`)
4. `exec "$@"` hands control to the user command

### PATH Order

```
/usr/local/silex/bin   ← wrappers: cp (reflink), tar (pigz/pixz), sort (--parallel), git (--depth 1)
/opt/llvm/bin          ← clang, clang++, lld
/opt/mold/bin          ← mold
/opt/sccache/bin       ← sccache
/usr/bin               ← system tools (busybox applets, xxhsum, pigz, etc.)
```

### DNS Pre-Warming

At build time, common host IPs are baked into `/etc/hosts` (marked
`# silex-dns-cache`). The entrypoint refreshes them in a background subshell
on container start, so DNS lookups for `apk.cgr.dev`, `github.com`,
`pypi.org`, etc. are fast even before the first `apk add`.

---

## 6. Compatibility Layer

### apt-shim

`/usr/local/bin/apt-get` is a POSIX sh script. On `apt-get install`:
1. Strips apt-specific flags (`-y`, `--no-install-recommends`, etc.)
2. Strips version specifiers (`pkg=1.2.3` → `pkg`)
3. Looks up each package in `config/package-mapping.json` (504 mappings)
4. Invokes `apk add --no-cache <translated-packages>` with
   `silex-nosync.so` preloaded

Symlinked as `/usr/local/bin/apt` for compatibility.

### Debian Shims

Five scripts in `/usr/local/silex/shims/`, symlinked to `/usr/local/bin/`:

| Shim | Behaviour |
|------|-----------|
| `adduser` | Translates Debian flags to busybox adduser |
| `addgroup` | Translates Debian flags to busybox addgroup |
| `lsb_release` | Returns Wolfi / rolling |
| `dpkg-architecture` | Returns amd64 / arm64 from `uname -m` |
| `update-alternatives` | No-op (Wolfi uses direct symlinks) |

`ID_LIKE=debian` is appended to `/etc/os-release` so scripts that check
`/etc/os-release` see a Debian-like environment.

---

## 7. Performance Characteristics

### Where silex Helps Most

1. **C++ projects with many translation units**: mold's advantage is largest
   on projects with large, few executables (Rust binaries, C++ monoliths)
   where linking is the critical path.

2. **Repeated builds in CI**: sccache + BuildKit cache mounts. After the
   first build, unchanged translation units cost ~2ms (cache lookup) vs
   ~420ms (compile). Measured: 44s → 2.5s for nlohmann/json (18x faster).

3. **Package install time**: `apk add` takes ~1s for update + 1-3s for
   packages. `apt-get update` alone takes 10-30s.

### Where silex Doesn't Help

1. **Single-file amalgamations** (e.g. SQLite): clang -O2/-O3 is slower
   than gcc on 230k-line single TUs. Set `CC=gcc` for these files.
   `silex lint <Dockerfile> <srcdir>` detects large TUs automatically.

2. **Pure interpreted workloads** (Python, Node): no compilation involved.

3. **Network-bound builds**: bottleneck is network, not compiler.

### Measured Numbers

Build times including package install. Docker 29.2.0, Linux 6.17.0,
x86_64, 32 cores, cold cache, 2 runs averaged.

```
Project           silex       ubuntu      speedup    alpine      speedup
nlohmann/json     1,600ms     50,843ms    31.8x      74,438ms    46.1x
fmtlib            1,423ms     20,170ms    14.1x
googletest        1,172ms      7,017ms     5.9x
abseil-cpp        1,350ms      7,814ms     5.7x
google/re2        1,061ms      1,206ms     1.1x                  (tie)
SQLite amalgam   12,728ms      1,268ms     0.1x                  SLOWER
```

Incremental build with warm sccache: 2.4s vs 44s cold (18x).

---

## 8. Security

### Source Integrity

Every source tarball is verified against a SHA256 value in `sources.json`
before compilation. Run `make verify-sources` to re-verify.

### No GPLv2 in silex:slim

apk-tools and busybox are GPLv2. Source is available at the upstream repos
referenced in `sources.json`. Written offer in `docs/LICENSING.md`.

silex:slim contains make (GPLv3) and bash (GPLv3) but no GPLv2 components.
silex:dev adds git (GPLv2).

### SBOM

The Wolfi/Chainguard packages installed by users at runtime (via `apk add`)
include SPDX JSON SBOMs at `/var/lib/db/sbom/`. silex core tools are
source-built; their provenance is `sources.json` + the Dockerfile.
