# Building Silex

Quick reference for building the Silex images after fixing package names for Wolfi.

## Changes Made to Dockerfile.slim

### Package Names Fixed

Original (broken):
- `compiler-rt` - Not a separate package in Wolfi
- `libcxx`, `libcxx-dev` - Wrong naming convention
- `sccache` - Not in Wolfi repository
- `dash` - Not in Wolfi repository
- `xxhash` - Not in Wolfi repository
- `python-3` - Version-specific naming required

Corrected to:
- Removed `compiler-rt` (included with clang-18)
- Changed to `libc++-18` and `libc++-18-dev`
- Added sccache download in builder stage (GitHub release binary)
- Removed dash (deferred to v0.1, use bash for MVP)
- Removed xxhash (not critical, skipped for MVP)
- Changed to `python-3.12` (version-specific)

### Builder Stage Additions

Added sccache download after uutils build:
- Downloads musl-linked static binary from GitHub releases
- Version 0.9.1 (check for updates at github.com/mozilla/sccache/releases)
- Strips binary to reduce size

### ENV Fixes

Removed:
```dockerfile
ENV MAKEFLAGS="-j$(nproc 2>/dev/null || echo 4)"
```

Reason: Shell expansion doesn't work in ENV directives. The entrypoint script already handles this correctly.

### PATH Updates

Changed from `/opt/mold/bin` only to `/opt/mold/bin:/opt/sccache/bin` to include sccache.

## First Build

```bash
cd silex
docker build -f dockerfiles/Dockerfile.slim -t silex:slim .
```

Expected build time: 15-25 minutes (mostly mold and uutils compilation).

## Likely Failures and Fixes

### Package Name Still Wrong

Error: `unable to select packages: clang-18 (no such package)`

Fix: Check actual package name in Wolfi:
```bash
docker run --rm cgr.dev/chainguard/wolfi-base apk search clang
```

Adjust Dockerfile with correct name (might be `clang-19` or `clang` without version).

### libc++ Package Not Found

Error: `unable to select packages: libc++-18 (no such package)`

Fix: Check Wolfi's libc++ naming:
```bash
docker run --rm cgr.dev/chainguard/wolfi-base apk search libc++
```

Might be `libc++`, `libc++-19`, or `libcxx` (without hyphen). Update Dockerfile accordingly.

### Python Path Wrong

Error: No error, but Python stripping fails silently.

Fix: After build succeeds, check actual Python path:
```bash
docker run --rm silex:slim find /usr/lib -name "python3.*" -type d | head -5
```

Update lines 134-143 with correct path (might be `/usr/lib/python3.13` or similar).

### sccache Download 404

Error: `curl: (22) The requested URL returned error: 404`

Fix: Check latest sccache release:
```bash
curl -s https://api.github.com/repos/mozilla/sccache/releases/latest | grep tag_name
```

Update `SCCACHE_VERSION` ARG in Dockerfile.

### LLVM Path Wrong

No error, but stripping does nothing.

Fix: After build, check where LLVM actually lives:
```bash
docker run --rm silex:slim find /usr -name "llvm*" -type d 2>/dev/null | head -5
```

Update line 108 with correct path (might be `/usr/lib/llvm18` or `/usr/lib/llvm-19`).

## Verification After Build

Check image exists and size:
```bash
docker images silex:slim
```

Target: Under 700MB. Ideal: 400-500MB.

Check tools are present and work:
```bash
docker run --rm silex:slim clang --version
docker run --rm silex:slim mold --version
docker run --rm silex:slim ninja --version
docker run --rm silex:slim sccache --version
docker run --rm silex:slim python3 --version
```

Check mold is symlinked as default linker:
```bash
docker run --rm silex:slim ls -la /usr/bin/ld
```

Should show symlink to `/opt/mold/bin/mold`.

Check environment variables are set:
```bash
docker run --rm silex:slim env | grep -E 'CC|CXX|CMAKE|SILEX'
```

Should see CC=clang, CXX=clang++, CMAKE_GENERATOR=Ninja, etc.

## Test Compilation

Compile the benchmark project manually:
```bash
docker run --rm \
    -v "$(pwd)/benchmarks/projects/cpp-json-parser:/work" \
    -w /work \
    silex:slim \
    bash -c "cmake -B build && cmake --build build"
```

If this succeeds, the toolchain works. Proceed to run the full benchmark script.

If it fails, check:
1. Is clang being used? (Check build output for "clang++" invocations)
2. Is mold being used? (Check for "ld.mold" or "mold" in linker lines)
3. Is ninja being used? (Check for "ninja" in build output, not "make")
4. Is sccache caching? (Run build twice, second should be faster if cached)

Check sccache stats:
```bash
docker run --rm silex:slim sccache --show-stats
```

## Debug Build Layers

See which layers are fat:
```bash
docker history silex:slim --human --no-trunc | head -20
```

If a layer is unexpectedly large (>200MB), identify what added the size:
```bash
# Check what was installed in that layer
docker run --rm silex:slim du -sh /usr/lib/* | sort -h | tail -10
docker run --rm silex:slim du -sh /usr/share/* | sort -h | tail -10
```

## Common Size Culprits

If image is >700MB, likely causes:
1. LLVM/clang not stripped (check /usr/lib/llvm*)
2. Python tests not removed (check /usr/lib/python3.*/test)
3. Locales not removed (check /usr/share/i18n)
4. Documentation not removed (check /usr/share/doc)

Fix by ensuring the stripping RUN commands actually ran. Check paths are correct for Wolfi's directory structure.

## Run Benchmark

Once basic compilation works:
```bash
cd benchmarks
./benchmark.sh
```

This builds both silex:slim and ubuntu:24.04 images, then compares build times. Target: 2-3x faster.

## Iteration Strategy

First build: Get it to complete without errors.

Second build: Verify tools work and environment is configured.

Third build: Reduce size if over 700MB.

Fourth build: Optimize paths and stripping for actual Wolfi structure.

Then benchmark. If <2x speedup, investigate why (likely sccache not caching, or mold not being used).

## Package Name Reference

When build fails with "no such package", search Wolfi's repository:

Method 1 (from container):
```bash
docker run -it cgr.dev/chainguard/wolfi-base
apk update
apk search <package>
```

Method 2 (GitHub):
Browse https://github.com/wolfi-dev/os and look for `<package>.yaml` files.

Method 3 (package repository):
Check https://packages.wolfi.dev/os (if accessible).

Note: Chainguard's `cgr.dev/chainguard/wolfi-base` may use a different repository (https://apk.cgr.dev/chainguard) with fewer packages than the community Wolfi repo. If a package is missing, you may need to build from source in the builder stage.

## Next Steps After Successful Build

1. Update README.md with actual benchmark results
2. Document actual Python and LLVM paths found in Wolfi
3. Consider adding dash and xxhash in v0.1 if size budget allows
4. Pin package versions for reproducibility
5. Proceed to v0.1 features (apt-get shim, silex doctor, etc.)
