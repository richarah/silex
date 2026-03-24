# Licensing

Silex is MIT licensed. It bundles or downloads the following third-party tools:

## silex:slim

| Tool | Version | License | Source |
|------|---------|---------|--------|
| Wolfi base | rolling | Apache 2.0 | chainguard |
| clang/LLVM | 18.1.8 | Apache 2.0 + LLVM exceptions | llvm.org |
| mold | 2.35.0 | MIT | rui314/mold |
| sccache | 0.8.2 | Apache 2.0 | mozilla/sccache |
| ninja | 1.13.2 | Apache 2.0 | ninja-build/ninja |
| tini | 0.19.0 | MIT | krallin/tini |
| jemalloc | 5.3.0 | BSD 2-Clause | jemalloc/jemalloc |
| mimalloc | 1.9.7 | MIT | microsoft/mimalloc |
| pigz | 2.8 | zlib | madler/pigz |
| pixz | 1.0.7 | BSD 2-Clause | vasi/pixz |
| fd | 10.4.2 | MIT | sharkdp/fd |
| ripgrep | 15.1.0 | MIT/Unlicense | BurntSushi/ripgrep |
| uv | 0.10.12 | MIT/Apache 2.0 | astral-sh/uv |
| xxhash | 0.8.3 | BSD 2-Clause | Cyan4973/xxHash |
| dash | 0.5.13 | BSD | Various |
| zstd | 1.5.7 | BSD 3-Clause | facebook/zstd |
| Python | 3.12 | PSFv2 | python.org |
| curl | 8.19.0 | curl (MIT-like) | curl.se |
| ca-certificates | 20251003 | MPL 2.0 + various | mozilla.org |
| make | 4.4.1 | GPLv3 | gnu.org/software/make |
| bash | 5.3 | GPLv3 | gnu.org/software/bash |

silex:slim contains **no GPLv2 components**. make and bash are GPLv3.
zstd is dual-licensed BSD 3-Clause / GPLv2; Silex uses the BSD 3-Clause licence.

## silex:dev (additional)

| Tool | Version | License | Source |
|------|---------|---------|--------|
| git | 2.53.0 | GPLv2 | git-scm.com |
| openssh-client | — | BSD/OpenSSL | openssh.com |

## GPLv3 in silex:slim (make, bash)

GPLv3 requires source availability for distributed binaries, same as GPLv2
but with additional anti-tivoization provisions. For `make` and `bash` in
silex:slim:

Source is available via the Wolfi build definitions at
https://github.com/wolfi-dev/os, which reference the exact upstream source
tarballs for each pinned version. Direct upstream sources:
- bash: https://ftp.gnu.org/gnu/bash/
- make: https://ftp.gnu.org/gnu/make/

## GPLv2 in silex:dev (git)

`git` is only present in silex:dev, not silex:slim.

For source availability: the Wolfi build definition for git is at
https://github.com/wolfi-dev/os (search `git.yaml`), which references the
exact upstream tarball. Direct upstream source: https://github.com/git/git

This does not constitute legal advice. For strict GPL compliance in
production distribution, consult a lawyer.

## zstd dual-licence note

zstd is dual-licensed BSD 3-Clause / GPLv2. Silex uses the BSD 3-Clause
licence. Both the library (libzstd) and the CLI tools are covered by the
dual-licence scheme; choosing BSD is valid.
