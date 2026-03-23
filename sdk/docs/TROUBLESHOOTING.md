# Silex Troubleshooting

Common issues and their fixes.

---

## Build Failures

### `Package not found` from apk

```
ERROR: unable to find a satisfying package for the specification 'foo'
```

**Cause**: The package name differs between Debian and Wolfi.

**Fix**:
1. Let the apt-shim translate it: `apt-get install foo` (checks 449 mappings automatically)
2. Search directly: `apk search foo`
3. Add a PR to expand `config/package-mapping.json`

---

### cmake can't find Ninja

```
CMake Error: Could not create named generator Ninja
```

**Cause**: `ninja` isn't in PATH. Wolfi's package installs to `/usr/lib/ninja-build/bin/ninja`.

**Fix**: Silex symlinks `/usr/bin/ninja` automatically. If this fails:

```bash
ln -sf /usr/lib/ninja-build/bin/ninja /usr/bin/ninja
```

---

### clang: unsupported option `-march=native`

**Cause**: `-march=native` doesn't work in cross-compilation contexts.

**Fix**: Replace with explicit arch: `-march=x86-64-v3` for modern x86_64, or remove the flag.

---

## Linker Issues

### mold + sccache: cache misses every time

**Cause**: Known issue [mozilla/sccache#1755](https://github.com/mozilla/sccache/issues/1755). mold's response file handling can break sccache's hash.

**Workaround**: Switch to lld:

```dockerfile
ENV SILEX_LINKER=lld
```

Or disable sccache if you don't need it:

```dockerfile
ENV SILEX_CACHE=off
```

---

### `cannot find -lstdc++` or `cannot find -lc++`

**Cause**: libc++ or libstdc++ header/library missing.

**Fix**:

```dockerfile
RUN apk add --no-cache libstdc++ libcxx1-18
```

---

### `/usr/bin/ld: error: unknown argument '--color-diagnostics'`

**Cause**: mold doesn't support all GNU ld flags.

**Fix**: Remove `--color-diagnostics` from your `LDFLAGS`, or switch linker:

```dockerfile
ENV SILEX_LINKER=lld
```

---

## Memory Allocator

### jemalloc not loading

```
silex: Warning: jemalloc requested but library not found, using system allocator
```

**Cause**: `libjemalloc.so*` not found in `/usr/lib` or `/usr/local/lib`.

**Diagnosis**:

```bash
docker run --rm silex:slim find /usr/lib -name 'libjemalloc*'
```

**Fix**: If the file is present but LD_PRELOAD isn't set, the entrypoint's find might be failing. Run `silex doctor` to check:

```bash
docker run --rm silex:slim silex doctor
```

---

### LD_PRELOAD conflicts

If your application preloads its own libraries, jemalloc may conflict.

**Fix**: Disable jemalloc:

```dockerfile
ENV SILEX_MALLOC=system
```

---

## Locale Issues

### `locale: Cannot set LC_ALL to default locale`

**Cause**: `LC_ALL=C` is set by Silex, but a tool expects `UTF-8`.

**Fix**: Override in your Dockerfile:

```dockerfile
ENV LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
```

---

### Python output has encoding errors

**Cause**: `PYTHONDONTWRITEBYTECODE=1` is set; unrelated to locale, but check `PYTHONIOENCODING`:

```dockerfile
ENV PYTHONIOENCODING=utf-8
```

---

## Wrapper Issues

### Sort takes much longer than expected

**Cause**: The sort wrapper adds `--parallel=nproc`, but the system sort is busybox (doesn't support `--parallel`). The wrapper detects this and falls back automatically.

**Diagnosis**: Check which sort is running:

```bash
docker run --rm silex:slim which sort  # should be /usr/local/silex/bin/sort
docker run --rm silex:slim sort --version
```

---

### cp fails with unexpected errors

**Cause**: The cp wrapper tries `--reflink=auto` for copy-on-write. On filesystems that don't support reflinks (ext4, most CI Docker volumes), this silently falls back to regular copy.

**Fix**: If cp is failing, disable wrappers:

```dockerfile
ENV SILEX_WRAPPERS=off
```

---

### git shallow clone fails in air-gapped environments

**Cause**: The git wrapper passes `--depth=1` for all clones by default.

**Fix**: Disable git shallow cloning:

```dockerfile
ENV SILEX_GIT_SHALLOW=off
```

---

## Debugging

### Run without the entrypoint

To start a raw shell without silex-entrypoint reconfiguring the environment:

```bash
docker run --rm --entrypoint /bin/bash silex:slim
```

### Disable all Silex features

```bash
docker run --rm \
  -e SILEX_MALLOC=system \
  -e SILEX_WRAPPERS=off \
  -e SILEX_APT_SHIM=off \
  -e SILEX_GIT_SHALLOW=off \
  -e SILEX_LINKER=ld \
  -e SILEX_CACHE=off \
  silex:slim bash
```

### silex doctor

Always start debugging with:

```bash
docker run --rm silex:slim silex doctor
```

This prints the full configuration: compiler, linker, allocator, cache, wrappers, and apt-shim status.

---

## Reporting Bugs

Open an issue at https://github.com/richarah/silex/issues. Include:
1. Output of `silex doctor`
2. The Dockerfile that fails
3. Full error output
4. Host OS and Docker version (`docker version`)
