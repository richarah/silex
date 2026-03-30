# tar builtin: feasibility study

**Date:** 2026-03-30
**Author:** matchbox project

---

## Summary

This document evaluates the feasibility of adding a `tar` builtin to matchbox. The conclusion is:

> **Feasible with a self-contained POSIX.1-2001 implementation (~2,000 lines C). Deferred to v0.3.0. Compression codec integration deferred further.**

---

## What tar must support (container build use case)

Container image layers and toolchain distribution archives use a small subset of tar features:

| Feature | Format | Required? |
|---------|--------|-----------|
| Create/extract POSIX `ustar` archives | POSIX.1-2001 | **Yes** |
| Long filenames (GNU `@LongLink` or PAX `path=`) | PAX/GNU | **Yes** |
| Symlinks, hardlinks | ustar | **Yes** |
| Devices, fifos, sockets | ustar | Optional |
| Permissions, uid/gid | ustar | **Yes** |
| `gzip` decompression | external | **Yes** (via pipe: `gzip -d` or `zcat`) |
| `bzip2` / `xz` / `zstd` decompression | external | Optional |
| In-process compression/decompression | internal codec | No (use `-z -f >(gzip)` shell idiom) |
| GNU tar extensions (sparse files) | GNU | No |
| Incremental archives | GNU | No |

### Conclusion on compression

Container builds pipe tar through `gzip`, `zstd`, or `xz`. matchbox does not need an internal codec if it delegates via subprocess (e.g. `tar czf` opens `gzip -c`). This is what GNU tar itself does for many formats. A `tar` builtin that opens a child `gzip`/`zstd` process avoids a codec library dependency.

---

## Implementation approaches

### Option A: Self-contained POSIX ustar (~2,000 lines)

Implement the POSIX ustar header format (512-byte blocks) in C, with:
- `tar c[z][f]` — create archive (optionally pipe through `gzip`)
- `tar x[z][f]` — extract archive (optionally pipe through `gzip -d`)
- `tar t[z][f]` — list archive
- `tar r[f]` — append to archive (non-compressed only)
- Long name extension: write PAX `path=` / `size=` headers for paths > 100 chars
- Long link extension: PAX `linkpath=` for symlinks > 100 chars
- Octal encoding/decoding of size, mtime, mode, uid, gid fields
- Blocking factor: 20 × 512-byte blocks (10 KB) per write

**Pros:**
- No external library dependency
- Fits matchbox architecture (self-contained, `~2,000` lines of C11)
- Handles 99%+ of container build layer archives
- Reproducible: no libarchive version differences

**Cons:**
- GNU tar extensions (sparse, multi-volume) not supported
- Compression requires subprocess (acceptable: `gzip` is also a matchbox builtin)

**Estimated LOC:** 1,800–2,200 lines (ustar read/write + PAX long-name extension)

### Option B: Thin libarchive wrapper (~300 lines)

Link against `libarchive` (`-larchive`) and wrap `archive_read_*` / `archive_write_*`.

**Pros:**
- Full GNU tar / PAX / POSIX compatibility
- All compression codecs (gzip, bzip2, xz, zstd, lz4) via libarchive's built-in support
- Very small wrapper code

**Cons:**
- Breaks the "static single binary" contract — adds libarchive + zlib + liblzma as dependencies
- musl-static link is difficult with libarchive (requires static versions of all codec libs)
- libarchive license (BSD-2) is compatible, but the dependency chain is complex
- Binary size: libarchive static adds ~1.4 MB; with codecs ~3–4 MB (exceeds our 2 MB target)

**Verdict: Rejected** for v0.3.0. May revisit as an optional loadable module (so the main binary stays lean).

### Option C: External `tar` subprocess via exec

When `tar` is invoked inside matchbox scripts, exec the system `tar`. This is the current behavior (fallthrough to system PATH).

**Cons:** Requires system `tar` installed; breaks "single binary" goal; no `#!/usr/bin/matchbox tar` shebang.

**Verdict: Not a builtin — status quo, acceptable as interim.**

---

## ustar header layout (reference)

```
Offset  Size  Field
0       100   name (NUL-terminated, or PAX-extended)
100     8     mode (octal ASCII)
108     8     uid
116     8     gid
124     12    size (octal ASCII)
136     12    mtime (octal ASCII, POSIX epoch seconds)
148     8     checksum (sum of header bytes, octal ASCII)
156     1     type flag: 0=regular, 1=hardlink, 2=symlink, 3=char, 4=block, 5=dir, 6=fifo
157     100   linkname
257     6     "ustar" magic
263     2     version "00"
265     32    uname
297     32    gname
329     8     devmajor
337     8     devminor
345     155   prefix (extends `name` for paths > 100 chars — POSIX ustar only)
500     12    padding
```

PAX extended header (type='x') precedes the entry it describes and contains
`key=value\n` records for fields that overflow the fixed ustar sizes.

---

## Implementation plan (v0.3.0)

### Phase 1: ustar read/write core (~800 lines)
- `tar_header_t` struct with encode/decode functions
- `tar_read_entry()` / `tar_write_entry()` operating on a fd (or FILE*)
- Checksum computation (simple byte sum with checksum field set to spaces)
- Octal field encode/decode

### Phase 2: PAX long-name support (~400 lines)
- Detect names/links > 100/157 bytes
- Emit PAX extended header (`path=`, `linkpath=`, `size=`, `mtime=` for sub-second precision)
- Parse PAX extended headers on extraction

### Phase 3: CLI layer (~500 lines)
- `tar [ctxr][zvf] [ARCHIVE] [FILE...]` argument parsing
- `-C DIR` support (chdir before extraction)
- `--strip-components=N` for layer extraction
- `-v` verbose listing
- `-z` compression dispatch (fork `gzip`/`gzip -d` subprocess via pipe)

### Phase 4: Tests (~200 lines)
- `tests/compat/run.sh` additions: create, extract, list, round-trip
- Compare output against GNU tar for representative archives

### Total: ~1,900 lines

---

## Decision

| Criterion | Score |
|-----------|-------|
| Container build support | High (ustar + PAX covers OCI layer format) |
| Implementation complexity | Medium (~2k lines, self-contained) |
| Test coverage risk | Medium (tar edge cases are numerous) |
| Binary size impact | Low (~15 KB compiled, within budget) |
| Dependency footprint | Zero (self-contained) |

**Recommendation:** Implement Option A (self-contained ustar + PAX) in v0.3.0 as a new `src/core/tar.c`. Compression via subprocess (gzip is already a matchbox builtin, so `tar czf` works in-process when gzip is available on PATH).

---

## Related files

| File | Role |
|------|------|
| `src/core/tar.c` | To be created in v0.3.0 |
| `src/main.c` | Register `applet_tar` |
| `tests/compat/run.sh` | Add tar compat tests |
| `docs/TAR_FEASIBILITY.md` | This document |
