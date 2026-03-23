# Migrating to Silex

How to migrate your existing Dockerfiles to `FROM silex:slim`.

---

## From `ubuntu:24.04` (most common)

This is the most common migration. The apt-get shim handles most of it automatically.

### Before

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    python3 \
    libssl-dev \
    git \
    curl

WORKDIR /workspace
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

### After

```dockerfile
FROM ghcr.io/richarah/silex:slim

# apt-get works (shim translates to apk).
# clang, cmake, ninja, sccache, mold already installed — no need to reinstall.
RUN apt-get install -y libssl-dev

WORKDIR /workspace
COPY . .
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

### What changed

1. `build-essential`, `cmake`, `ninja-build` are already in the image — remove them.
2. `apt-get update` is automatic in the shim — remove it.
3. Add `--mount=type=cache` for sccache to persist across builds.
4. Compiler is now clang (not gcc). If you need gcc: `apk add gcc` and `ENV SILEX_CC=gcc`.

---

## From `alpine:latest`

Alpine uses musl; Silex uses glibc. Some packages have different names.

### Before

```dockerfile
FROM alpine:latest

RUN apk add --no-cache \
    clang \
    cmake \
    ninja \
    python3 \
    openssl-dev \
    git \
    curl

WORKDIR /workspace
COPY . .
RUN cmake -B build && cmake --build build
```

### After

```dockerfile
FROM ghcr.io/richarah/silex:slim

# Most Alpine package names work directly in Wolfi.
# clang, cmake, ninja, git, curl, python3 are already installed.
RUN apk add --no-cache openssl-dev

WORKDIR /workspace
COPY . .
RUN cmake -B build && cmake --build build
```

### What changed

1. clang, cmake, ninja, git, curl, python3 are preinstalled — remove them.
2. Existing `apk add` commands work as-is (same package names).
3. Binaries compiled with musl may not work in Silex (glibc) — recompile from source.

---

## From `debian:bookworm`

Same as Ubuntu — apt-get shim handles the translation.

### Before

```dockerfile
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang \
    cmake \
    ninja-build \
    python3 \
    libssl-dev

COPY . /src
WORKDIR /src
RUN cmake -B build && cmake --build build
```

### After

```dockerfile
FROM ghcr.io/richarah/silex:slim

# clang, cmake, ninja are preinstalled.
RUN apt-get install -y libssl-dev

COPY . /src
WORKDIR /src
RUN cmake -B build && cmake --build build
```

---

## Common Gotchas

### 1. `CC=gcc` or `CXX=g++` hardcoded

Silex defaults to clang. If your Makefile or CMake config hardcodes GCC:

```dockerfile
# Option A: Install gcc and switch
RUN apk add --no-cache gcc g++
ENV SILEX_CC=gcc SILEX_CXX=g++

# Option B: Keep clang but set compatibility flags
ENV CC=clang CXX=clang++
```

### 2. `/bin/sh` is bash on Ubuntu, dash on Wolfi

If your scripts use `#!/bin/sh` with bash extensions (arrays, `[[ ]]`), they'll fail. Fix options:
- Change the shebang to `#!/bin/bash`
- Or avoid bash extensions (use POSIX `[ ]` instead of `[[ ]]`)

### 3. `python` vs `python3`

Silex installs `python3`. The `python` command may not exist:

```dockerfile
# Add a symlink if needed
RUN ln -sf /usr/bin/python3 /usr/bin/python
```

### 4. `locale` commands fail

Silex sets `LC_ALL=C` and strips most locale data. Build tools work fine, but locale-dependent tools may behave differently. If you need full locale support:

```dockerfile
ENV LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
```

### 5. Package names differ

Use the apt-shim, which handles 449 common translations. For edge cases:

```bash
# Find the Wolfi package name
docker run --rm silex:slim apk search <package-name>

# Or look up in the mapping file
cat /usr/local/silex/package-mapping.json | python3 -c "
import json, sys
m = json.load(sys.stdin)
print(m.get('your-package-name', 'not mapped'))
"
```

### 6. `sudo` not available

Wolfi containers run as root by default. `sudo` is not installed and not needed.

---

## When Silex is NOT the Right Choice

Silex is optimised for build images. It's a bad fit for:

1. **Runtime images**: Use `silex:runtime` or `FROM scratch` / `FROM distroless`. The ~500MB build image should never ship to production.

2. **Simple shell scripts**: If your Dockerfile just runs a Python script with no compilation, the overhead of a 500MB image isn't worth it.

3. **Projects that depend on specific GCC versions**: Silex uses clang-18. GCC can be added, but if the project heavily depends on GCC-specific behaviour, test carefully.

4. **Proprietary or platform-specific packages**: If you depend on a Debian-only `.deb` package with no equivalent in Wolfi, migration may not be feasible.

5. **ARM64 in v0.1**: ARM64 build support is included in the Dockerfile, but testing has only been done on x86_64. CI/CD multi-arch builds come in v0.2.
