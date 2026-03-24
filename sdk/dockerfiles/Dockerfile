# Silex v2 self-hosted build.
# Requires a previous silex:slim image. Faster than bootstrap (~15-20 min vs ~90-120 min).
# Produces identical output to Dockerfile.bootstrap.
# LLVM and mold are carried forward from the previous silex:slim; not rebuilt.
# To upgrade LLVM or mold, run Dockerfile.bootstrap instead.
#
# Usage:
#   docker build -f dockerfiles/Dockerfile -t silex:slim .
#
# ============================================================================

# ============================================================================
# Source versions. Must match sources.json.
# ============================================================================

ARG NINJA_VERSION=1.12.1
ARG NINJA_SHA256=821bdff48a3f683bc4bb3b6f0b5fe7b2d647cf65d52aeb63328c91a6c6df285a
ARG ZSTD_VERSION=1.5.6
ARG ZSTD_SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
ARG XXHASH_VERSION=0.8.2
ARG XXHASH_SHA256=baee0c6afd4f03165de7a4e67988d16f0f2b257b51d0e3cb91909302a26a79c4
ARG MIMALLOC_VERSION=2.1.7
ARG MIMALLOC_SHA256=0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d
ARG APK_VERSION=2.14.4
ARG APK_SHA256=c8ad16bddfae988392ad067a244cfdaf643eb7d6761b73450739585ca76f9a25
ARG BUSYBOX_VERSION=1.37.0
ARG BUSYBOX_SHA256=3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4
ARG DASH_VERSION=0.5.12
ARG DASH_SHA256=6a474ac46e8b0b32916c4c60df694c82058d3297d8b385b74508030ca4a8f28a
ARG TINI_VERSION=0.19.0
ARG TINI_SHA256=0fd35a7030052acd9f58948d1d900fe1e432ee37103c5561554408bdac6bbf0d
ARG PIGZ_VERSION=2.8
ARG PIGZ_SHA256=eb872b4f0e1f0ebe59c9f7bd8c506c4204893ba6a8492de31df416f0d5170fd0
ARG PIXZ_VERSION=1.0.7
ARG PIXZ_SHA256=e5e32c6eb0bf112b98e74a5da8fb63b9f2cae71800f599d97ce540e150c8ddc5
ARG COREUTILS_VERSION=9.5
ARG COREUTILS_SHA256=cd328edeac92f6a665de9f323c93b712af1858bc2e0d88f3f7100469470a1b8a
ARG SCCACHE_VERSION=0.8.2
ARG SCCACHE_SHA256=2b3e0ef8902fe7bcdcfccf393e29f4ccaafc0194cbb93681eaac238cdc9b94f8
ARG FD_VERSION=10.2.0
ARG FD_SHA256=73329fe24c53f0ca47cd0939256ca5c4644742cb7c14cf4114c8c9871336d342
ARG RG_VERSION=14.1.1
ARG RG_SHA256=4dad02a2f9c8c3c8d89434e47337aa654cb0e2aa50e806589132f186bf5c2b66
ARG UV_VERSION=0.4.30
ARG UV_SHA256=0f74a570e52affd82ff7987ade2553e144a326e7ff58de24ab4242999a3dc931
ARG RUST_VERSION=1.82.0
ARG RUST_SHA256_X86_64=8a6bf24ca1fee2bd861590431c7954674191a0c0dfa0af4169731bc1459cf58a
ARG RUST_SHA256_AARCH64=99acf175be33bd96ac82d644125d0e2acc6962d61ee64851edc7d8ba3bd23ca4

ARG SILEX_PGO=0
ARG TARGETARCH

# ============================================================================
# Stage: build-c
# Uses previous silex:slim — clang and mold already present.
# Rebuilds all C/C++ tools from source using the existing toolchain.
# LLVM and mold are NOT rebuilt; they are carried forward via COPY below.
# ============================================================================
FROM silex:slim AS build-c

ARG NINJA_VERSION NINJA_SHA256
ARG ZSTD_VERSION ZSTD_SHA256
ARG XXHASH_VERSION XXHASH_SHA256
ARG MIMALLOC_VERSION MIMALLOC_SHA256
ARG APK_VERSION APK_SHA256
ARG BUSYBOX_VERSION BUSYBOX_SHA256
ARG DASH_VERSION DASH_SHA256
ARG TINI_VERSION TINI_SHA256
ARG PIGZ_VERSION PIGZ_SHA256
ARG PIXZ_VERSION PIXZ_SHA256
ARG COREUTILS_VERSION COREUTILS_SHA256
ARG SILEX_PGO
ARG TARGETARCH

RUN mkdir -p /src /build /out/bin /out/lib /out/share

# ============================================================================
# Compiler environment. Inherits clang + mold from silex:slim PATH.
# ============================================================================
ENV CC="clang"
ENV CXX="clang++"
ENV CFLAGS="-O3 -fomit-frame-pointer"
ENV CXXFLAGS="-O3 -fomit-frame-pointer"
ENV LDFLAGS="-fuse-ld=/opt/mold/bin/mold"

ARG MARCH_FLAG="-march=x86-64-v3"
ENV CFLAGS="${CFLAGS} ${MARCH_FLAG}"
ENV CXXFLAGS="${CXXFLAGS} ${MARCH_FLAG}"

# ============================================================================
# sccache wrapper: accelerate C/C++ compilation in the self-hosted build.
# Disabled with SILEX_SCCACHE=off or if sccache is not found.
# ============================================================================
ENV SCCACHE_DIR="/root/.cache/sccache"
ENV RUSTC_WRAPPER="sccache"
ENV CC="sccache clang"
ENV CXX="sccache clang++"

# ============================================================================
# Download and verify all C/C++ source tarballs.
# ============================================================================
RUN set -ex \
    \
    && curl -fsSL "https://github.com/ninja-build/ninja/archive/refs/tags/v${NINJA_VERSION}.tar.gz" \
        -o /src/ninja.tar.gz \
    && echo "${NINJA_SHA256}  /src/ninja.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" \
        -o /src/zstd.tar.gz \
    && echo "${ZSTD_SHA256}  /src/zstd.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/Cyan4973/xxHash/archive/refs/tags/v${XXHASH_VERSION}.tar.gz" \
        -o /src/xxhash.tar.gz \
    && echo "${XXHASH_SHA256}  /src/xxhash.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/microsoft/mimalloc/archive/refs/tags/v${MIMALLOC_VERSION}.tar.gz" \
        -o /src/mimalloc.tar.gz \
    && echo "${MIMALLOC_SHA256}  /src/mimalloc.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://gitlab.alpinelinux.org/alpine/apk-tools/-/archive/v${APK_VERSION}/apk-tools-v${APK_VERSION}.tar.gz" \
        -o /src/apk-tools.tar.gz \
    && echo "${APK_SHA256}  /src/apk-tools.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL --retry 5 --retry-delay 5 "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2" \
        -o /src/busybox.tar.bz2 \
    && echo "${BUSYBOX_SHA256}  /src/busybox.tar.bz2" | sha256sum -c - \
    \
    && curl -fsSL "http://gondor.apana.org.au/~herbert/dash/files/dash-${DASH_VERSION}.tar.gz" \
        -o /src/dash.tar.gz \
    && echo "${DASH_SHA256}  /src/dash.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/krallin/tini/archive/refs/tags/v${TINI_VERSION}.tar.gz" \
        -o /src/tini.tar.gz \
    && echo "${TINI_SHA256}  /src/tini.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://zlib.net/pigz/pigz-${PIGZ_VERSION}.tar.gz" \
        -o /src/pigz.tar.gz \
    && echo "${PIGZ_SHA256}  /src/pigz.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/vasi/pixz/releases/download/v${PIXZ_VERSION}/pixz-${PIXZ_VERSION}.tar.xz" \
        -o /src/pixz.tar.xz \
    && echo "${PIXZ_SHA256}  /src/pixz.tar.xz" | sha256sum -c - \
    \
    && curl -fsSL "https://ftp.gnu.org/gnu/coreutils/coreutils-${COREUTILS_VERSION}.tar.xz" \
        -o /src/coreutils.tar.xz \
    && echo "${COREUTILS_SHA256}  /src/coreutils.tar.xz" | sha256sum -c -

# ============================================================================
# Build ninja (cmake generator for subsequent builds).
# ============================================================================
RUN set -ex \
    && tar xf /src/ninja.tar.gz -C /build \
    && cd /build/ninja-${NINJA_VERSION} \
    && cmake -B _build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/out \
        -DBUILD_TESTING=OFF \
    && cmake --build _build -j$(nproc) \
    && cmake --install _build \
    && strip --strip-unneeded /out/bin/ninja \
    && rm -rf /build/ninja-${NINJA_VERSION}

ENV PATH="/out/bin:${PATH}"

# ============================================================================
# Build zstd.
# ============================================================================
RUN set -ex \
    && tar xf /src/zstd.tar.gz -C /build \
    && cd /build/zstd-${ZSTD_VERSION} \
    && cmake -S build/cmake -B _build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/out \
        -DZSTD_BUILD_STATIC=OFF \
        -DZSTD_BUILD_PROGRAMS=ON \
        -DZSTD_BUILD_TESTS=OFF \
    && ninja -C _build -j$(nproc) \
    && ninja -C _build install \
    && strip --strip-unneeded /out/bin/zstd \
    && rm -rf /build/zstd-${ZSTD_VERSION}

# ============================================================================
# Build xxhash (library + xxhsum binary).
# ============================================================================
RUN set -ex \
    && tar xf /src/xxhash.tar.gz -C /build \
    && cd /build/xxHash-${XXHASH_VERSION} \
    && make -j$(nproc) \
    && make install PREFIX=/out \
    && strip --strip-unneeded /out/bin/xxhsum 2>/dev/null || true \
    && rm -rf /build/xxHash-${XXHASH_VERSION}

# ============================================================================
# Build mimalloc (shared library only, for LD_PRELOAD).
# ============================================================================
RUN set -ex \
    && tar xf /src/mimalloc.tar.gz -C /build \
    && cd /build/mimalloc-${MIMALLOC_VERSION} \
    && cmake -B _build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/out \
        -DMI_BUILD_STATIC=OFF \
        -DMI_BUILD_OBJECT=OFF \
        -DMI_BUILD_TESTS=OFF \
    && ninja -C _build -j$(nproc) \
    && ninja -C _build install \
    && rm -rf /build/mimalloc-${MIMALLOC_VERSION}

# ============================================================================
# Build tini (PID 1 reaper).
# ============================================================================
RUN set -ex \
    && tar xf /src/tini.tar.gz -C /build \
    && cd /build/tini-${TINI_VERSION} \
    && cmake -B _build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/out \
    && ninja -C _build -j$(nproc) \
    && install -m755 _build/tini /out/bin/tini \
    && strip --strip-unneeded /out/bin/tini \
    && rm -rf /build/tini-${TINI_VERSION}

# ============================================================================
# Build dash (/bin/sh).
# ============================================================================
RUN set -ex \
    && tar xf /src/dash.tar.gz -C /build \
    && cd /build/dash-${DASH_VERSION} \
    && ./configure \
        --bindir=/bin \
        --prefix=/out \
        CC="${CC}" \
        CFLAGS="${CFLAGS}" \
        LDFLAGS="${LDFLAGS}" \
    && make -j$(nproc) \
    && make install \
    && strip --strip-unneeded /out/bin/dash 2>/dev/null || true \
    && rm -rf /build/dash-${DASH_VERSION}

# ============================================================================
# Build busybox (dynamic, glibc, defconfig).
# ============================================================================
RUN set -ex \
    && tar xf /src/busybox.tar.bz2 -C /build \
    && cd /build/busybox-${BUSYBOX_VERSION} \
    && make defconfig \
    && make -j$(nproc) \
        CC="${CC}" \
        EXTRA_CFLAGS="${CFLAGS}" \
        EXTRA_LDFLAGS="${LDFLAGS}" \
    && install -m755 busybox /out/bin/busybox \
    && strip --strip-unneeded /out/bin/busybox \
    && rm -rf /build/busybox-${BUSYBOX_VERSION}

# ============================================================================
# Build pigz (parallel gzip).
# ============================================================================
RUN set -ex \
    && tar xf /src/pigz.tar.gz -C /build \
    && cd /build/pigz-${PIGZ_VERSION} \
    && make -j$(nproc) CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" \
    && install -m755 pigz /out/bin/pigz \
    && strip --strip-unneeded /out/bin/pigz \
    && rm -rf /build/pigz-${PIGZ_VERSION}

# ============================================================================
# Build pixz (parallel xz).
# ============================================================================
RUN set -ex \
    && tar xf /src/pixz.tar.xz -C /build \
    && cd /build/pixz-${PIXZ_VERSION} \
    && autoreconf -fi \
    && ./configure \
        --prefix=/out \
        CC="${CC}" \
        CFLAGS="${CFLAGS}" \
        LDFLAGS="${LDFLAGS}" \
    && make -j$(nproc) \
    && make install \
    && strip --strip-unneeded /out/bin/pixz \
    && rm -rf /build/pixz-${PIXZ_VERSION}

# ============================================================================
# Build GNU coreutils — sort binary only (--parallel support).
# ============================================================================
RUN set -ex \
    && tar xf /src/coreutils.tar.xz -C /build \
    && cd /build/coreutils-${COREUTILS_VERSION} \
    && ./configure \
        --prefix=/out \
        CC="${CC}" \
        CFLAGS="${CFLAGS}" \
        LDFLAGS="${LDFLAGS}" \
    && make -j$(nproc) src/sort \
    && install -m755 src/sort /out/bin/sort-parallel \
    && strip --strip-unneeded /out/bin/sort-parallel \
    && rm -rf /build/coreutils-${COREUTILS_VERSION}

# ============================================================================
# Build apk-tools.
# ============================================================================
RUN set -ex \
    && tar xf /src/apk-tools.tar.gz -C /build \
    && cd /build/apk-tools-v${APK_VERSION} \
    && ./autogen.sh \
    && ./configure \
        --prefix=/out \
        --without-lua \
        --without-doc \
        CC="${CC}" \
        CFLAGS="${CFLAGS}" \
        LDFLAGS="${LDFLAGS}" \
    && make -j$(nproc) \
    && make install \
    && APK_BIN=$(find /out -name apk -type f 2>/dev/null | head -1) \
    && strip --strip-unneeded "${APK_BIN}" \
    && install -m755 "${APK_BIN}" /out/bin/apk \
    && rm -rf /build/apk-tools-v${APK_VERSION}

# ============================================================================
# Build silex-nosync.so.
# ============================================================================
COPY src/silex-nosync.c /src/silex-nosync.c
RUN clang -O2 -shared -fPIC -nostartfiles \
        -o /out/lib/silex-nosync.so \
        /src/silex-nosync.c \
    && strip /out/lib/silex-nosync.so

# ============================================================================
# Stage: build-rust
# Uses silex:slim for build tools. Downloads fresh Rust toolchain.
# Uses mold (from silex:slim) instead of lld for faster link.
# Runs in parallel with build-c (no shared dependencies).
# ============================================================================
FROM silex:slim AS build-rust

ARG RUST_VERSION RUST_SHA256_X86_64 RUST_SHA256_AARCH64
ARG SCCACHE_VERSION SCCACHE_SHA256
ARG FD_VERSION FD_SHA256
ARG RG_VERSION RG_SHA256
ARG UV_VERSION UV_SHA256
ARG TARGETARCH

RUN mkdir -p /src /out/bin

# Download Rust toolchain (pinned, architecture-specific).
RUN set -ex \
    && ARCH=$([ "${TARGETARCH}" = "arm64" ] && echo "aarch64" || echo "x86_64") \
    && RUST_SHA256=$([ "${TARGETARCH}" = "arm64" ] && echo "${RUST_SHA256_AARCH64}" || echo "${RUST_SHA256_X86_64}") \
    && curl -fsSL "https://static.rust-lang.org/dist/rust-${RUST_VERSION}-${ARCH}-unknown-linux-gnu.tar.xz" \
        -o /src/rust.tar.xz \
    && echo "${RUST_SHA256}  /src/rust.tar.xz" | sha256sum -c - \
    && tar xf /src/rust.tar.xz -C /src \
    && /src/rust-${RUST_VERSION}-${ARCH}-unknown-linux-gnu/install.sh \
        --prefix=/opt/rust \
        --disable-ldconfig \
    && rm -rf /src/rust*

ENV PATH="/opt/rust/bin:${PATH}"
# Use mold for Rust linking — available in silex:slim.
ENV RUSTFLAGS="-C target-cpu=x86-64-v3 -C lto=thin -C link-arg=-fuse-ld=/opt/mold/bin/mold"

# Download and verify Rust tool sources.
RUN set -ex \
    && curl -fsSL "https://github.com/mozilla/sccache/archive/refs/tags/v${SCCACHE_VERSION}.tar.gz" \
        -o /src/sccache.tar.gz \
    && echo "${SCCACHE_SHA256}  /src/sccache.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/sharkdp/fd/archive/refs/tags/v${FD_VERSION}.tar.gz" \
        -o /src/fd.tar.gz \
    && echo "${FD_SHA256}  /src/fd.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/BurntSushi/ripgrep/archive/refs/tags/${RG_VERSION}.tar.gz" \
        -o /src/ripgrep.tar.gz \
    && echo "${RG_SHA256}  /src/ripgrep.tar.gz" | sha256sum -c - \
    \
    && curl -fsSL "https://github.com/astral-sh/uv/archive/refs/tags/${UV_VERSION}.tar.gz" \
        -o /src/uv.tar.gz \
    && echo "${UV_SHA256}  /src/uv.tar.gz" | sha256sum -c -

RUN set -ex \
    && tar xf /src/sccache.tar.gz -C /src \
    && cd /src/sccache-${SCCACHE_VERSION} \
    && cargo build --release --locked \
    && install -m755 target/release/sccache /out/bin/sccache \
    && strip --strip-unneeded /out/bin/sccache

RUN set -ex \
    && tar xf /src/fd.tar.gz -C /src \
    && cd /src/fd-${FD_VERSION} \
    && cargo build --release --locked \
    && install -m755 target/release/fd /out/bin/fd \
    && strip --strip-unneeded /out/bin/fd

RUN set -ex \
    && tar xf /src/ripgrep.tar.gz -C /src \
    && cd /src/ripgrep-${RG_VERSION} \
    && cargo build --release --locked \
    && install -m755 target/release/rg /out/bin/rg \
    && strip --strip-unneeded /out/bin/rg

RUN set -ex \
    && tar xf /src/uv.tar.gz -C /src \
    && cd /src/uv-${UV_VERSION} \
    && cargo build --release --locked \
    && install -m755 target/release/uv /out/bin/uv \
    && strip --strip-unneeded /out/bin/uv

# ============================================================================
# Stage: final
# Identical to bootstrap final stage.
# Base: debian:bookworm-slim (glibc, libssl3, zlib1g, ca-certificates).
# LLVM and mold are copied from the PREVIOUS silex:slim, not rebuilt.
# ============================================================================
FROM debian:bookworm-slim AS final

ARG TARGETARCH
# Extract LLVM major version from the image we carry forward.
# silex:slim carries /opt/llvm; we read the version at build time via --build-arg.
ARG LLVM_MAJOR=18

# Install only what the final image needs from Debian.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && rm -rf /usr/share/doc /usr/share/man /usr/share/info

# Remove apt and dpkg. The apt-get shim replaces them.
RUN dpkg --force-remove-reinstreq --purge apt apt-utils 2>/dev/null || true \
    && rm -rf /var/lib/apt /var/cache/apt /usr/lib/apt \
    && rm -f /usr/bin/apt /usr/bin/apt-get /usr/bin/apt-cache \
    && rm -rf /var/lib/dpkg/info/apt* /var/lib/dpkg/info/apt-utils*

# ============================================================================
# LLVM and mold: carry forward from previous silex:slim.
# ============================================================================
COPY --from=silex:slim /opt/llvm/ /opt/llvm/
COPY --from=silex:slim /opt/mold/ /opt/mold/

# ============================================================================
# Copy freshly built tools from builder stages.
# ============================================================================

# Core tools.
COPY --from=build-c /out/bin/busybox    /bin/busybox
COPY --from=build-c /out/bin/dash       /bin/dash
COPY --from=build-c /out/bin/tini       /usr/bin/tini
COPY --from=build-c /out/bin/zstd       /usr/bin/zstd
COPY --from=build-c /out/bin/xxhsum     /usr/bin/xxhsum
COPY --from=build-c /out/bin/pigz       /usr/bin/pigz
COPY --from=build-c /out/bin/pixz       /usr/bin/pixz
COPY --from=build-c /out/bin/ninja      /usr/bin/ninja

# Libraries.
COPY --from=build-c /out/lib/silex-nosync.so /usr/local/silex/lib/silex-nosync.so

# mimalloc shared library.
COPY --from=build-c /out/lib/libmimalloc* /usr/lib/

# Silex-internal binaries.
RUN mkdir -p /usr/local/silex/bin
COPY --from=build-c /out/bin/sort-parallel /usr/local/silex/bin/sort-parallel

# apk package manager.
COPY --from=build-c /out/bin/apk        /usr/bin/apk

# sccache compiler cache.
COPY --from=build-rust /out/bin/sccache /opt/sccache/bin/sccache

# Rust tools.
COPY --from=build-rust /out/bin/fd  /usr/bin/fd
COPY --from=build-rust /out/bin/rg  /usr/bin/rg
COPY --from=build-rust /out/bin/uv  /usr/bin/uv

# ============================================================================
# Configure busybox: install applets as symlinks.
# ============================================================================
RUN /bin/busybox --install -s /bin \
    && ln -sf /bin/dash /bin/sh \
    && ln -sf /bin/busybox /usr/bin/which \
    && ln -sf /bin/busybox /usr/bin/find \
    && ln -sf /bin/busybox /usr/bin/awk \
    && ln -sf /bin/busybox /usr/bin/sed

# Clang symlinks.
RUN ln -sf /opt/llvm/bin/clang   /usr/bin/clang \
    && ln -sf /opt/llvm/bin/clang++ /usr/bin/clang++ \
    && ln -sf /opt/llvm/bin/clang   /usr/bin/cc \
    && ln -sf /opt/llvm/bin/clang++ /usr/bin/c++ \
    && ln -sf /opt/llvm/bin/clang   "/usr/bin/clang-${LLVM_MAJOR}" \
    && ln -sf /opt/llvm/bin/clang++ "/usr/bin/clang++-${LLVM_MAJOR}" \
    && ln -sf /opt/llvm/bin/lld     /usr/bin/lld \
    && ln -sf /opt/llvm/bin/ld.lld  /usr/bin/ld.lld \
    && ln -sf /opt/mold/bin/mold    /usr/bin/mold \
    && ln -sf /opt/mold/bin/mold    /usr/bin/ld \
    && ln -sf /opt/mold/bin/mold    /usr/bin/ld.mold

# ============================================================================
# Configure apk for Wolfi (Chainguard) repositories.
# ============================================================================
COPY --from=cgr.dev/chainguard/wolfi-base:latest /etc/apk/keys/ /etc/apk/keys/

RUN mkdir -p /etc/apk /var/cache/apk /var/lib/apk /lib/apk/db \
    && echo "https://apk.cgr.dev/chainguard" > /etc/apk/repositories \
    && touch /lib/apk/db/installed \
    && apk update

# ============================================================================
# Set ID_LIKE=debian so tools that check /etc/os-release see a Debian-like env.
# ============================================================================
RUN echo 'ID_LIKE=debian' >> /etc/os-release

# ============================================================================
# Install Silex scripts.
# ============================================================================
COPY scripts/apt-shim          /usr/local/bin/apt-get
COPY scripts/entrypoint.sh     /usr/local/bin/silex-entrypoint
COPY scripts/silex             /usr/local/bin/silex
COPY scripts/shims/            /usr/local/silex/shims/
COPY scripts/wrappers/         /usr/local/silex/bin/
COPY config/package-mapping.json /usr/local/silex/package-mapping.json

RUN chmod +x /usr/local/bin/apt-get \
    /usr/local/bin/silex-entrypoint \
    /usr/local/bin/silex \
    && chmod +x /usr/local/silex/shims/* \
    && chmod +x /usr/local/silex/bin/* \
    && ln -sf /usr/local/bin/apt-get /usr/local/bin/apt \
    && for f in /usr/local/silex/shims/*; do \
           ln -sf "$f" "/usr/local/bin/$(basename "$f")"; \
       done

# ============================================================================
# DNS pre-warming.
# ============================================================================
RUN for _host in \
        apk.cgr.dev \
        packages.wolfi.dev \
        github.com \
        objects.githubusercontent.com \
        pypi.org \
        registry.npmjs.org \
        crates.io \
        proxy.golang.org; do \
    _ip=$(getent ahosts "$_host" 2>/dev/null | awk '/STREAM/{print $1;exit}'); \
    [ -n "$_ip" ] && echo "$_ip $_host  # silex-dns-cache" >> /etc/hosts; \
    done || true

# ============================================================================
# Remove locale and timezone data.
# ============================================================================
RUN find /usr/share/locale -mindepth 1 -maxdepth 1 \
        ! -name 'C' ! -name 'en_US' ! -name 'C.UTF-8' \
        -exec rm -rf {} + 2>/dev/null || true \
    && find /usr/share/zoneinfo -mindepth 1 -maxdepth 1 \
        ! -name 'UTC' ! -name 'Europe' ! -name 'America' \
        ! -name 'zone.tab' ! -name 'iso3166.tab' \
        -exec rm -rf {} + 2>/dev/null || true

# Hardlink dedup.
RUN find /usr /opt -xdev -type f -size +4k 2>/dev/null \
    | sort \
    | xargs sha256sum 2>/dev/null \
    | sort -k1,1 \
    | awk 'prev==$1{printf "%s\0%s\0",pf,$NF}{prev=$1;pf=$NF}' \
    | xargs -0 -r -n2 ln -f \
    || true

# ============================================================================
# Environment.
# ============================================================================
ENV PATH="/usr/local/silex/bin:/opt/llvm/bin:/opt/mold/bin:/opt/sccache/bin:$PATH"

ENV CC="clang"
ENV CXX="clang++"
ENV LD="mold"
ENV CMAKE_GENERATOR="Ninja"

ENV RUSTC_WRAPPER="sccache"
ENV SCCACHE_IDLE_TIMEOUT="0"
ENV SCCACHE_CACHE_SIZE="10G"

ENV SILEX_MALLOC="mimalloc"

ENV LC_ALL="C"
ENV LANG="C.UTF-8"
ENV TZ="UTC"
ENV CURL_HTTP_VERSION="2TLS"

ENV NINJA_STATUS="[%f/%t %es] "

ARG SOURCE_DATE_EPOCH=1735689600
ENV SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH}"
ENV SILEX_IMAGE_VERSION="slim-2.0.0"

LABEL maintainer="richarah@github"
LABEL org.opencontainers.image.title="Silex"
LABEL org.opencontainers.image.description="Docker base image for fast builds. Everything from source."
LABEL org.opencontainers.image.version="2.0.0"
LABEL org.opencontainers.image.licenses="MIT"
LABEL org.opencontainers.image.source="https://github.com/richarah/silex"

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/silex-entrypoint"]
CMD ["/bin/bash"]
