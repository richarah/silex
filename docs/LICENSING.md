# Licensing

Silex is MIT licensed. The following third-party tools are compiled from
source or installed at build time. All SHA256 values are in `sources.json`.

## silex:slim — compiled from source

All tools below are compiled from source during `make bootstrap`. The Rust
toolchain (1.82.0) is used only during the build stage and is not present
in the final image.

| Tool | Version | License | Source |
|------|---------|---------|--------|
| clang/LLVM | 18.1.8 | Apache-2.0 WITH LLVM-exception | llvm.org |
| mold | 2.40.4 | MIT | rui314/mold |
| ninja | 1.12.1 | Apache-2.0 | ninja-build/ninja |
| zstd | 1.5.6 | BSD-3-Clause | facebook/zstd |
| xxhash | 0.8.2 | BSD-2-Clause | Cyan4973/xxHash |
| mimalloc | 2.1.7 | MIT | microsoft/mimalloc |
| tini | 0.19.0 | MIT | krallin/tini |
| dash | 0.5.12 | BSD-3-Clause | gondor.apana.org.au |
| pigz | 2.8 | zlib | zlib.net/pigz |
| pixz | 1.0.7 | BSD-2-Clause | vasi/pixz |
| busybox | 1.37.0 | GPL-2.0-only | busybox.net |
| apk-tools | 2.14.4 | GPL-2.0-only | alpinelinux.org/apk-tools |
| coreutils sort | 9.5 | GPL-3.0-or-later | gnu.org/software/coreutils |
| sccache | 0.8.2 | Apache-2.0 | mozilla/sccache |
| fd | 10.2.0 | MIT OR Apache-2.0 | sharkdp/fd |
| ripgrep | 14.1.1 | MIT OR Unlicense | BurntSushi/ripgrep |
| uv | 0.4.30 | MIT OR Apache-2.0 | astral-sh/uv |

## silex:slim — installed via apt (debian:bookworm-slim base packages)

These packages are installed via `apt-get` from the Debian bookworm archive
during the final stage build. They provide glibc and TLS runtime.

| Package | License | Notes |
|---------|---------|-------|
| ca-certificates | MPL-2.0 + various | Mozilla root CAs |
| libssl3 | OpenSSL / SSLeay | TLS runtime for apk-tools |
| zlib1g | zlib | Compression runtime for apk-tools |

## silex:dev (additional, installed via apk)

| Tool | Version | License | Source |
|------|---------|---------|--------|
| git | 2.53.0 | GPL-2.0-only | git-scm.com |
| openssh-client | — | BSD/OpenSSL | openssh.com |

git is the only GPLv2 component in the silex image family.

---

## GPL notes

### busybox and apk-tools (GPL-2.0-only) in silex:slim

Both busybox and apk-tools are licensed GPL-2.0-only. Source is available:
- busybox: https://busybox.net/downloads/ (busybox-1.37.0.tar.bz2)
- apk-tools: https://gitlab.alpinelinux.org/alpine/apk-tools/-/archive/v2.14.4/

The exact tarballs used are recorded with SHA256 values in `sources.json`.
This constitutes a written offer of source.

### coreutils sort (GPL-3.0-or-later) in silex:slim

Only the `sort` binary is installed, as `/usr/local/silex/bin/sort-parallel`.
It is not exposed on PATH directly; only the silex sort wrapper calls it.
Source: https://ftp.gnu.org/gnu/coreutils/coreutils-9.5.tar.xz

### make and bash (GPL-3.0-or-later) in silex:slim

Installed via apk from the Wolfi/Chainguard repository.
Source availability via the Wolfi build definitions:
https://github.com/wolfi-dev/os — search `make.yaml` and `bash.yaml`.
Direct upstream sources: https://ftp.gnu.org/gnu/make/ and
https://ftp.gnu.org/gnu/bash/

### git (GPL-2.0-only) in silex:dev only

`git` is not present in silex:slim.

Source availability: https://github.com/wolfi-dev/os (git.yaml) and
https://github.com/git/git (tag v2.53.0).

### zstd dual-licence

zstd is dual-licensed BSD-3-Clause / GPL-2.0. Silex uses the BSD-3-Clause
licence. The library (libzstd) and the CLI tool are both covered by the
dual-licence scheme; choosing BSD is valid.

---

This does not constitute legal advice. For strict GPL compliance in
production distribution, consult a lawyer.
