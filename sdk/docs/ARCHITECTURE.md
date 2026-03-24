# Silex Architecture

A technical deep-dive into how Silex is built and why each decision was made.

---

## 1. Base Layer Design

### Why Wolfi?

Silex uses [Wolfi](https://github.com/wolfi-dev) (by Chainguard) as its base, not Alpine or Ubuntu.

| Property | Wolfi | Alpine | Ubuntu 24.04 |
|----------|-------|--------|--------------|
| Base size | 14MB | 7MB | 77MB |
| libc | glibc 2.43 | musl | glibc |
| CVE posture | Zero CVEs (daily updates) | Moderate | Moderate |
| Scanner support | All major scanners | Good | Good |
| Package freshness | Daily rebuilds | Varies | LTS freeze |
| Container-native | Yes | Yes | No |

**Why not Alpine?** Alpine uses musl libc, which has subtle performance differences from glibc during compilation (particularly with ThinLTO and large template-heavy C++ codebases). Pre-built toolchain binaries (mold, sccache) target glibc. Alpine is excellent for runtime images; less ideal for build images.

**Why not Ubuntu/Debian?** `apt-get update` takes 10-30 seconds. Ubuntu ships ~800 packages for a "minimal" install. Postinstall scripts (locale-gen, man-db rebuilds, dpkg triggers) run during every `RUN apt-get install`. In a multi-stage build environment, these are pure waste.

**Why not scratch/distroless?** Build images need shell, package manager, and build tools. Scratch is for runtime.

### Multi-Stage Build

The Dockerfile uses two stages:

```
builder  → Downloads mold and sccache prebuilt binaries (curl-only)
final    → apk installs all packages, then copies binaries from builder
```

The builder stage keeps the final image clean: no curl, no downloaded tarballs, no intermediate files.

### Package Selection Strategy

Every package in `apk add` is justified:

| Package | Reason |
|---------|--------|
| `clang-18` | Compiler (better with mold/ThinLTO than gcc) |
| `libcxx1-18` + `libcxx1-18-dev` | libc++ for C++ builds |
| `ninja-build` | Fast build system (lower overhead than Make) |
| `python-3.12` | Required by many build systems (Meson, GN, wheel builds) |
| `ca-certificates` | HTTPS for downloading dependencies at build time |
| `tini` | Signal reaping for container entrypoint |
| `zstd` | Fast compression for layer caching and artifact packaging |
| `curl` | Downloading dependencies at build time |
| `git` | Source checkout and submodule fetching |
| `make` | Fallback for projects that require GNU Make |
| `bash` | Entrypoint script requires bash; common build scripts use bash |
| `jemalloc` | Faster memory allocator, loaded via LD_PRELOAD |

### Size Optimisation

The Dockerfile applies six size-reduction passes after installation:

1. **Strip LLVM shared objects**: `strip --strip-unneeded` on all `.so*` files under `/usr/lib/llvm*`. Saves ~100MB.
2. **Strip locale data**: Keep only `en_US` and `C`. glibc ships ~200MB of locales; we reduce to ~2MB.
3. **Strip timezone data**: Keep UTC, Europe/, America/. Reduces tzdata from ~20MB to ~5MB.
4. **Strip Python**: Remove `test/`, `idlelib/`, `tkinter/`, `turtledemo/`, `ensurepip/`, all `__pycache__/`. Reduces Python from ~80MB to ~30MB.
5. **Strip documentation**: Remove `/usr/share/doc`, `man`, `info`, `gtk-doc`, `help`. Saves ~50-100MB.
6. **Clear package cache**: `rm -rf /var/cache/apk/*`.

---

## 2. Tool Selection Rationale

### Compiler: clang-18

clang is chosen over gcc because:
- Better integration with the LLVM toolchain (mold uses LLVM IR for LTO; sccache has better clang support)
- Single binary can target multiple architectures without separate cross-compiler packages
- ThinLTO is more mature and faster in clang than in gcc
- `clang-18` is a specific version (not floating `clang`) for reproducibility

gcc is not excluded by design — users can `apk add gcc` and set `SILEX_CC=gcc`.

### Linker: mold 2.35.0

mold is the default linker, symlinked as `/usr/bin/ld`:

| Linker | Relative speed | Notes |
|--------|---------------|-------|
| GNU ld | 1x | Baseline |
| gold | 2-5x | Better than ld, worse than mold |
| lld | 2-4x | Good; better sccache compatibility |
| mold | 5-10x | Fastest; parallel by design |

mold is downloaded as a prebuilt binary (not compiled from source) because compiling mold requires Rust + C++, which would add ~5 minutes and ~2GB to the builder stage.

Known issue: [mozilla/sccache#1755](https://github.com/mozilla/sccache/issues/1755) — mold can occasionally break sccache's hash-based caching. Workaround: `SILEX_LINKER=lld`.

### Build System: Ninja

Ninja has two advantages over Make:
1. **Better job scheduling**: Ninja computes the full dependency graph before starting; Make processes it incrementally.
2. **Lower overhead**: No recursive Make, no shell invocation per rule.

`CMAKE_GENERATOR=Ninja` is set so all CMake projects automatically use Ninja. Make remains available as fallback.

### Compilation Cache: sccache 0.8.2

sccache chosen over ccache because:
- Supports remote backends (S3, GCS, Redis, GitHub Actions cache) — important for CI/CD
- Works with Rust (RUSTC_WRAPPER)
- Better multi-language support
- Stateless: no daemon required

sccache is downloaded as a prebuilt musl binary, which runs on glibc systems without issues.

### Allocator: jemalloc

jemalloc is faster than glibc malloc for multi-threaded allocators (typical in compiler workloads). It's loaded via `LD_PRELOAD` by the entrypoint, transparently accelerating any process that uses malloc.

The entrypoint discovers the `.so` path at runtime: `find /usr/lib /usr/local/lib -name 'libjemalloc.so*' | head -1`. This avoids hardcoding a path that may change between package versions.

**jemalloc vs mimalloc**: both are installed. jemalloc is the current default. Benchmark plan: compile template-heavy C++ project (nlohmann/json) under each allocator, compare peak RSS and wall time. `SILEX_MALLOC=mimalloc` switches to mimalloc.

### Compression: zstd

zstd is default because:
- Parallel decompression (`ZSTD_NBTHREADS=0` = auto)
- Fast decompression: ~1.5 GB/s vs gzip's ~400 MB/s
- Level 3 is a good speed/size tradeoff for build artifacts
- Docker layer caching uses zstd when available

---

## 3. Environment Configuration

### SILEX_* Variable Flow

1. `tini` starts, sets up signal handling, then invokes `silex-entrypoint`
2. `silex-entrypoint` reads all `SILEX_*` env vars
3. Based on those vars, it sets `CC`, `CXX`, `LDFLAGS`, `CMAKE_GENERATOR`, `MAKEFLAGS`, `RUSTC_WRAPPER`, and `LD_PRELOAD`
4. Prints a one-line config summary to stderr (unless `SILEX_QUIET=on`)
5. `exec "$@"` — hands control to the user's command

All SILEX_* variables have documented defaults set in the Dockerfile `ENV` block. They can be overridden by users at any layer.

### PATH Order

```
/usr/local/silex/bin   ← wrappers (cp, tar, sort, git)
/opt/mold/bin          ← mold
/opt/sccache/bin       ← sccache
/usr/bin               ← system tools
...
```

Wrappers come first so they transparently intercept `cp`, `tar`, `sort`, and `git` calls without requiring Dockerfile changes.

### Symlink Management

- `/usr/bin/ld` → `/opt/mold/bin/mold` (transparent mold adoption)
- `/usr/bin/ld.mold` → `/opt/mold/bin/mold` (explicit reference)
- `/usr/bin/ninja` → `/usr/lib/ninja-build/bin/ninja` (Wolfi installs to non-standard path)
- `/usr/local/bin/apt` → `/usr/local/bin/apt-get` (apt alias)

---

## 4. Compatibility Layer (apt-shim)

### Problem

Most Dockerfiles have `RUN apt-get install foo`. Wolfi uses `apk`, not `apt-get`. Package names also differ (`build-essential` → `build-base`, `libssl-dev` → `openssl-dev`).

### Solution

`/usr/local/bin/apt-get` is a Python script that:
1. Parses `apt-get install <packages>` arguments
2. Looks up each package in `/usr/local/silex/package-mapping.json`
3. Translates mapped names, passes through unmapped names
4. Invokes `apk add <translated-packages>`

### Package Mapping

`config/package-mapping.json` contains 504 Debian→Wolfi mappings covering:
- Build tools (`build-essential`, `cmake`, `ninja-build`, etc.)
- Libraries (`libssl-dev`, `libz-dev`, `libpng-dev`, etc.)
- Languages (`python3`, `nodejs`, `golang`, etc.)
- System tools (`wget`, `unzip`, `file`, etc.)

When a package isn't in the mapping, the shim warns and passes the name through to `apk add` as-is (Wolfi may have it under the same name).

### Limitations

- `apt-get update`, `apt-get upgrade`, `apt-get remove` are translated to their `apk` equivalents
- `dpkg`, `apt-cache`, `debconf` are not shimmed
- Version pinning (`apt-get install foo=1.2.3`) is not supported (strips version)
- PPAs and external apt repositories have no equivalent

---

## 5. Performance Characteristics

### Where Silex Helps Most

1. **C++ projects with many translation units**: mold's parallel linking makes the biggest difference on projects with 100+ `.o` files. Linking `chromium` goes from 90s (GNU ld) to 8s (mold).

2. **Repeated builds in CI**: sccache provides object-level caching. With a warm cache, unchanged files are not recompiled. Combined with BuildKit's `--mount=type=cache`, incremental CI builds are **15-20x faster** (measured: 44s → 2.5s for nlohmann/json test suite).

3. **Python extension builds** (e.g., NumPy, PyTorch from source): jemalloc reduces allocator contention during multi-threaded compilation.

### Where Silex Doesn't Help Much

1. **Single-file projects**: Linking time is negligible; mold's advantage disappears.
2. **Interpreted language projects** (pure Python, Node.js): No compilation, so compiler/linker choices don't apply.
3. **Heavy network I/O builds** (downloading many deps): Bottleneck is network, not compilation.

### Measured Speedups

Benchmark: nlohmann/json full test suite (186 build steps, 84 test executables, 32-core host).

| Configuration | Cold build | vs. Ubuntu GCC-13 |
|---|---|---|
| Ubuntu 24.04 GCC-13 + Ninja + GNU ld | ~50s | baseline |
| Wolfi GCC-15 + Ninja + GNU ld | ~64s | 1.3x **slower** |
| Wolfi Clang-18 + Ninja + GNU ld | ~43s | 1.14x faster |
| Wolfi Clang-18 + Ninja + mold | ~43s | 1.14x faster |
| silex:slim (cold sccache) | ~44s | 1.1x faster |
| **silex:slim (warm sccache)** | **~2.5s** | **~20x faster** |

**Key findings:**

- **Compiler**: Clang-18 is 14-33% faster than GCC for template-heavy C++. Wolfi GCC-15 is paradoxically slower than Ubuntu GCC-13 because the OpenSSF hardening flags (`-ftrivial-auto-var-init=zero`, `-fstack-clash-protection`, `-fno-omit-frame-pointer`) add per-file compilation overhead.
- **Linker**: mold is neutral here — with 32 cores, the 84 link steps are parallelized with compilation and never become the bottleneck. mold's advantage is largest on projects with a few large executables (Rust binaries, large C++ monoliths) where linking IS the critical path.
- **sccache**: The dominant speedup for real developer workflows. Cold: ~44s. Warm: **~2.5s (18x faster)**. After the first build, any unchanged translation unit costs ~2ms (cache lookup) instead of ~420ms (compile).

Cold build speedups are workload-dependent and modest (10-33%). The compelling use case for silex is **incremental builds and CI caching**.

---

## 6. Security Model

### Zero CVE Base

Wolfi is rebuilt daily from source with the latest security patches. The Chainguard package repository (`apk.cgr.dev/chainguard`) provides signed packages.

All major vulnerability scanners work with Wolfi:
- Trivy: full support
- Grype: full support
- Docker Scout: full support
- Snyk: full support
- Wiz: full support

### SBOM

Wolfi packages include SPDX SBOM data in `/var/lib/db/sbom/`. Every installed package has a corresponding `.spdx.json` file.

### Reproducibility

- `SOURCE_DATE_EPOCH=1735689600` is set for reproducible builds
- All `apk add` packages are pinned to exact versions
- mold and sccache are downloaded at exact version numbers

### Rootless Operation

The image does not require `--privileged`. All operations in the entrypoint script use only filesystem writes to `/usr/bin` (for symlinks), which is writable in a standard Docker container.

---

## 7. Future Directions

### Coreutils

Toybox vs uutils: decision pending benchmark. Currently shipping Wolfi's default coreutils (BusyBox-compatible). [uutils](https://github.com/uutils/coreutils) (Rust, statically linked) is 20-50% faster on sort tasks. Deferred pending stability validation on C++ build workloads.

### GPU Acceleration (v0.2)

`silex:full` will add CUDA toolkit support for GPU-accelerated compression (nvCOMP) and hashing. Gated on Wolfi shipping CUDA packages or an acceptable NVIDIA base.

### Cross-Compilation (v1.0)

`Dockerfile.cross` will add sysroots and target toolchain wrappers for x86_64↔arm64 cross-compilation. See `dockerfiles/Dockerfile.cross`.
