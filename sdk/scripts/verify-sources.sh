#!/bin/sh
# verify-sources.sh — download all source tarballs, print SHA256.
# Run once before building to populate SHA256 values in sources.json and Dockerfile.bootstrap.
#
# Usage:
#   sh scripts/verify-sources.sh
#
# Output: lines in the form:
#   <name>  <sha256>  <url>
#
# Redirect to a file and review before updating sources.json.
# Uses companion .sha256 files where available (Rust, coreutils); downloads
# full tarballs for the rest.

set -e

WORKDIR=$(mktemp -d /tmp/silex-verify-XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

# Fetch SHA256 by downloading the full tarball and computing locally.
_sha256_download() {
    _name="$1"
    _url="$2"
    _file="$WORKDIR/$_name"
    printf "Downloading %s ... " "$_name" >&2
    curl -fsSL --retry 3 --retry-delay 2 "$_url" -o "$_file"
    _sum=$(sha256sum "$_file" | awk '{print $1}')
    printf "%s\n" "$_sum" >&2
    printf "%s  %s  %s\n" "$_name" "$_sum" "$_url"
}

# Fetch SHA256 from a companion .sha256 file (fast — tiny text file).
# Falls back to full download if companion file is absent or malformed.
_sha256_companion() {
    _name="$1"
    _url="$2"
    _sha_url="${_url}.sha256"
    printf "Fetching checksum %s ... " "$_name" >&2
    _raw=$(curl -fsSL --retry 3 --retry-delay 2 "$_sha_url" 2>/dev/null || true)
    # companion file format: "<hex>  filename" or just "<hex>"
    _sum=$(printf "%s" "$_raw" | awk 'NR==1{print $1}')
    if printf "%s" "$_sum" | grep -qE '^[0-9a-f]{64}$'; then
        printf "%s\n" "$_sum" >&2
        printf "%s  %s  %s\n" "$_name" "$_sum" "$_url"
    else
        printf "companion absent, downloading ... " >&2
        _sha256_download "$_name" "$_url"
    fi
}

LLVM_VERSION=18.1.8
MOLD_VERSION=2.40.4
NINJA_VERSION=1.12.1
ZSTD_VERSION=1.5.6
XXHASH_VERSION=0.8.2
MIMALLOC_VERSION=2.1.7
APK_VERSION=2.14.4
BUSYBOX_VERSION=1.37.0
DASH_VERSION=0.5.12
TINI_VERSION=0.19.0
PIGZ_VERSION=2.8
PIXZ_VERSION=1.0.7
COREUTILS_VERSION=9.5
SCCACHE_VERSION=0.8.2
FD_VERSION=10.2.0
RG_VERSION=14.1.1
UV_VERSION=0.4.30
RUST_VERSION=1.82.0

# LLVM: no companion file, must download (~120MB)
_sha256_download llvm \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/llvm-project-${LLVM_VERSION}.src.tar.xz"

_sha256_download mold \
    "https://github.com/rui314/mold/archive/refs/tags/v${MOLD_VERSION}.tar.gz"

_sha256_download ninja \
    "https://github.com/ninja-build/ninja/archive/refs/tags/v${NINJA_VERSION}.tar.gz"

_sha256_download zstd \
    "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz"

_sha256_download xxhash \
    "https://github.com/Cyan4973/xxHash/archive/refs/tags/v${XXHASH_VERSION}.tar.gz"

_sha256_download mimalloc \
    "https://github.com/microsoft/mimalloc/archive/refs/tags/v${MIMALLOC_VERSION}.tar.gz"

_sha256_download apk-tools \
    "https://gitlab.alpinelinux.org/alpine/apk-tools/-/archive/v${APK_VERSION}/apk-tools-v${APK_VERSION}.tar.gz"

_sha256_download busybox \
    "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"

_sha256_download dash \
    "http://gondor.apana.org.au/~herbert/dash/files/dash-${DASH_VERSION}.tar.gz"

_sha256_download tini \
    "https://github.com/krallin/tini/archive/refs/tags/v${TINI_VERSION}.tar.gz"

_sha256_download pigz \
    "https://zlib.net/pigz/pigz-${PIGZ_VERSION}.tar.gz"

_sha256_download pixz \
    "https://github.com/vasi/pixz/releases/download/v${PIXZ_VERSION}/pixz-${PIXZ_VERSION}.tar.xz"

# GNU coreutils: gnu.org publishes SHA256SUMS alongside tarballs
_sha256_companion coreutils \
    "https://ftp.gnu.org/gnu/coreutils/coreutils-${COREUTILS_VERSION}.tar.xz"

_sha256_download sccache \
    "https://github.com/mozilla/sccache/archive/refs/tags/v${SCCACHE_VERSION}.tar.gz"

_sha256_download fd \
    "https://github.com/sharkdp/fd/archive/refs/tags/v${FD_VERSION}.tar.gz"

_sha256_download ripgrep \
    "https://github.com/BurntSushi/ripgrep/archive/refs/tags/${RG_VERSION}.tar.gz"

_sha256_download uv \
    "https://github.com/astral-sh/uv/archive/refs/tags/${UV_VERSION}.tar.gz"

# Rust: static.rust-lang.org publishes companion .sha256 files (fast)
_sha256_companion rust-x86_64 \
    "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-x86_64-unknown-linux-gnu.tar.xz"

_sha256_companion rust-aarch64 \
    "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-aarch64-unknown-linux-gnu.tar.xz"

printf "\nDone. Paste SHA256 values into sources.json and Dockerfile.bootstrap ARG defaults.\n" >&2
