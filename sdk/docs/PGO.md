# PGO Build Pipeline (v1.0)

Profile-guided optimisation for the tools built from source in silex:slim.
Triples build time. Done once per release, binaries shipped to users.

## What gets PGO'd

Worth it (hot path, measurable gain):
- zstd: built-in PGO support (`make pgo`). Profiled on decompressing 50-100 Wolfi .apk packages.
- sccache: Rust, PGO via cargo-pgo. Profiled on compile + cache-miss + cache-hit cycle.

Not worth it:
- mold: already very fast; PGO overhead on linking would be marginal
- dash: too small, already near-zero startup overhead
- ninja: profile collection overhead > gain

## Workloads for profiling

### zstd
```sh
# Profile on the actual workload: decompress Wolfi packages
apk fetch -o /tmp/pkgs clang ninja make bash 2>/dev/null
for pkg in /tmp/pkgs/*.apk; do zstd -d "$pkg" -o /dev/null; done
```

### sccache
```sh
# Cold run (cache miss, store path)
sccache clang -O2 -c nlohmann/json/test/src/unit-json.cpp -o /tmp/out1.o
# Warm run (cache hit, retrieve path)
sccache clang -O2 -c nlohmann/json/test/src/unit-json.cpp -o /tmp/out2.o
```

## Dockerfile pipeline

The pipeline runs in the builder stage, not the final image:

```dockerfile
FROM cgr.dev/chainguard/wolfi-base AS pgo-builder

# 1. Build instrumented binaries
ARG ZSTD_VERSION=1.5.7
RUN apk add --no-cache clang make llvm
RUN curl -fsSL https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz \
      | tar xz && \
    cd zstd-${ZSTD_VERSION} && \
    make CC=clang CFLAGS="-fprofile-generate=/tmp/zstd.profraw" && \
    make install PREFIX=/opt/zstd-instr

# 2. Run representative workload to collect profiles
RUN apk fetch -o /tmp/pkgs clang ninja bash 2>/dev/null && \
    for pkg in /tmp/pkgs/*.apk; do \
        /opt/zstd-instr/bin/zstd -d "$pkg" -o /dev/null 2>/dev/null || true; \
    done

# 3. Merge raw profiles
RUN llvm-profdata merge /tmp/zstd.profraw/*.profraw -o /tmp/zstd.profdata

# 4. Rebuild with profile data
RUN cd zstd-${ZSTD_VERSION} && \
    make CC=clang CFLAGS="-fprofile-use=/tmp/zstd.profdata -fprofile-correction" \
         PREFIX=/opt/zstd-pgo install

# Final image copies /opt/zstd-pgo instead of installing from apk
```

## BOLT (post-link layout optimisation)

Applied after PGO. Reorders functions and basic blocks based on profile data.
5-15% additional gain over PGO per Meta's published numbers for clang.

```sh
# After PGO build:
llvm-bolt /opt/sccache/bin/sccache \
    --data=/tmp/sccache.fdata \
    --reorder-blocks=ext-tsp \
    --reorder-functions=hfsort+ \
    --split-functions \
    --split-all-cold \
    -o /opt/sccache/bin/sccache.bolt
mv /opt/sccache/bin/sccache.bolt /opt/sccache/bin/sccache
```

BOLT requires perf data collection or LLVM instrumentation. perf is not available in
containers by default; use LLVM instrumentation mode (`-instrument` flag).

## Expected gains

From published benchmarks on similar workloads:
- zstd decompression: 10-20% throughput increase
- sccache compilation: 5-15% (mostly startup path and cache lookup)
- Clang itself (if ever PGO'd): ~20% compile time reduction (LLVM docs)

These gains compound with the baseline sccache warm-cache speedup (21x).
PGO affects cold builds more than warm sccache builds.

## When to run

Once per major version. Triggered by the release pipeline, not CI. Results
are checked in as prebuilt binaries in the builder stage (not profiling runs).
