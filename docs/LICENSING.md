# Licensing

Silex is MIT licensed. It bundles or downloads the following third-party tools:

| Tool | Version | License | Source | Notes |
|------|---------|---------|--------|-------|
| Wolfi base | rolling | Apache 2.0 | chainguard | Base OS |
| clang/LLVM | 18.1.8 | Apache 2.0 + LLVM exceptions | llvm.org | |
| mold | 2.35.0 | MIT | rui314/mold | |
| sccache | 0.8.2 | Apache 2.0 | mozilla/sccache | |
| ninja | 1.13.2 | Apache 2.0 | ninja-build/ninja | |
| tini | 0.19.0 | MIT | krallin/tini | |
| jemalloc | 5.3.0 | BSD 2-Clause | jemalloc/jemalloc | |
| mimalloc | 1.9.7 | MIT | microsoft/mimalloc | |
| pigz | 2.8 | zlib | madler/pigz | |
| pixz | 1.0.7 | BSD 2-Clause | vasi/pixz | |
| fd | 10.4.2 | MIT | sharkdp/fd | |
| ripgrep | 15.1.0 | MIT/Unlicense | BurntSushi/ripgrep | |
| uv | 0.10.12 | MIT/Apache 2.0 | astral-sh/uv | |
| xxhash | 0.8.3 | BSD 2-Clause | Cyan4973/xxHash | |
| dash | 0.5.13 | BSD | Various | POSIX sh implementation |
| zstd | 1.5.7 | BSD 3-Clause | facebook/zstd | Dual BSD/GPLv2; BSD used |
| Python | 3.12 | PSFv2 | python.org | |
| curl | 8.19.0 | curl (MIT-like) | curl.se | |
| ca-certificates | 20251003 | MPL 2.0 + various | mozilla.org | Root certificate bundle |
| git | 2.53.0 | GPLv2 | git-scm.com | See GPL section below |
| make | 4.4.1 | GPLv3 | gnu.org/software/make | See GPL section below |
| bash | 5.3 | GPLv3 | gnu.org/software/bash | See GPL section below |

## GPL-licensed tools in silex:slim

`git` (GPLv2), `make` (GPLv3), and `bash` (GPLv3) are present in **silex:slim**. GPL
requires source availability for distributed binaries.

**What we can honestly say:**

Wolfi packages are built from source by Chainguard. Build definitions (melange YAML)
are published at [github.com/wolfi-dev/os](https://github.com/wolfi-dev/os) and
reference the exact upstream source tarballs for each pinned version. This serves as
a practical source availability mechanism.

However: Wolfi SBOMs in `/var/lib/db/sbom/` record `licenseConcluded: NOASSERTION`
for most packages and do not constitute a formal GPL written offer.

**For source code of GPL-licensed components in silex:**

> The source for `git`, `make`, and `bash` as built for silex can be found via the
> Wolfi build definitions at https://github.com/wolfi-dev/os, which reference the
> exact upstream tarballs. For direct upstream source:
> - git: https://github.com/git/git
> - make: https://ftp.gnu.org/gnu/make/
> - bash: https://ftp.gnu.org/gnu/bash/

**If you have strict GPL compliance requirements** (e.g., redistribution in a product),
consult a lawyer. The above is not legal advice.

## zstd dual-license note

zstd is dual-licensed BSD 3-Clause / GPLv2. Silex uses the BSD 3-Clause license.
The `zstd` CLI and library are both covered by BSD under the dual-license scheme.
