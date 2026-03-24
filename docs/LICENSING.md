# Licensing

Silex is MIT licensed. It bundles or downloads the following third-party tools:

| Tool | Version | License | Source | Notes |
|------|---------|---------|--------|-------|
| Wolfi base | rolling | Apache 2.0 | chainguard | Base OS |
| clang/LLVM | 18.1.8 | Apache 2.0 + LLVM exceptions | llvm.org | Compatible with MIT |
| mold | 2.35.0 | MIT | rui314/mold | |
| sccache | 0.8.2 | Apache 2.0 | mozilla/sccache | |
| ninja | 1.13.2 | Apache 2.0 | ninja-build/ninja | |
| tini | 0.19.0 | MIT | krallin/tini | |
| jemalloc | 5.3.0 | BSD 2-Clause | jemalloc/jemalloc | |
| mimalloc | 1.9.7 | MIT | microsoft/mimalloc | |
| pigz | 2.8 | zlib | madler/pigz | |
| pixz | 1.0.7 | BSD | vasi/pixz | |
| fd | 10.4.2 | MIT | sharkdp/fd | |
| ripgrep | 15.1.0 | MIT/Unlicense | BurntSushi/ripgrep | |
| uv | 0.10.12 | MIT/Apache 2.0 | astral-sh/uv | |
| xxhash | 0.8.3 | BSD 2-Clause | Cyan4973/xxHash | |
| dash | 0.5.13 | BSD/Other | Various | POSIX sh implementation |
| zstd | 1.5.7 | BSD/GPLv2 | facebook/zstd | Dual-licensed; BSD used |
| Python | 3.12 | PSFv2 | python.org | PSF license is permissive |
| git | 2.53.0 | GPLv2 | git-scm.com | GPLv2 binary distribution OK in containers |

## GPLv2 in MIT-licensed images

Docker images distribute binaries, not source. GPLv2 requires providing source for distributed binaries. Chainguard/Wolfi packages include SBOM data (SPDX JSON at `/var/lib/db/sbom/`) which points to upstream sources. This satisfies GPLv2 source availability requirements.
