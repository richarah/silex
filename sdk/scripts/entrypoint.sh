#!/bin/bash
# Silex entrypoint: reads SILEX_* environment variables and configures build environment
# This script is called by tini as the container entrypoint

set -e

# ============================================================================
# Configuration from SILEX_* environment variables
# ============================================================================

# Allocator configuration (must be first — sets LD_PRELOAD before anything else)
MALLOC="${SILEX_MALLOC:-jemalloc}"
case "$MALLOC" in
    jemalloc)
        _jemalloc_so=$(find /usr/lib /usr/local/lib -name 'libjemalloc.so*' 2>/dev/null | head -1)
        if [ -n "$_jemalloc_so" ]; then
            export LD_PRELOAD="$_jemalloc_so"
        else
            echo "silex: Warning: jemalloc requested but library not found, using system allocator" >&2
        fi
        ;;
    mimalloc)
        _mimalloc_so=$(find /usr/lib /usr/local/lib -name 'libmimalloc.so*' 2>/dev/null | sort | tail -1)
        if [ -n "$_mimalloc_so" ]; then
            export LD_PRELOAD="$_mimalloc_so"
        else
            echo "silex: Warning: mimalloc library not found, using system allocator" >&2
        fi
        ;;
    system)
        unset LD_PRELOAD
        ;;
    *)
        echo "silex: Warning: Unknown SILEX_MALLOC value '$MALLOC'. Using jemalloc." >&2
        _jemalloc_so=$(find /usr/lib /usr/local/lib -name 'libjemalloc.so*' 2>/dev/null | head -1)
        [ -n "$_jemalloc_so" ] && export LD_PRELOAD="$_jemalloc_so"
        ;;
esac

# Compiler configuration
export CC="${SILEX_CC:-clang}"
export CXX="${SILEX_CXX:-clang++}"

# Linker configuration
LINKER="${SILEX_LINKER:-mold}"
case "$LINKER" in
    mold)
        export LDFLAGS="-fuse-ld=mold"
        # Ensure mold is symlinked as ld
        if [ ! -L /usr/bin/ld ] || [ "$(readlink /usr/bin/ld)" != "/opt/mold/bin/mold" ]; then
            ln -sf /opt/mold/bin/mold /usr/bin/ld 2>/dev/null || true
        fi
        ;;
    lld)
        export LDFLAGS="-fuse-ld=lld"
        if command -v lld >/dev/null 2>&1; then
            ln -sf "$(command -v lld)" /usr/bin/ld 2>/dev/null || true
        fi
        ;;
    gold)
        export LDFLAGS="-fuse-ld=gold"
        if command -v gold >/dev/null 2>&1; then
            ln -sf "$(command -v gold)" /usr/bin/ld 2>/dev/null || true
        fi
        ;;
    ld)
        export LDFLAGS=""
        # Point to system ld.bfd if available
        if [ -f /usr/bin/ld.bfd ]; then
            ln -sf /usr/bin/ld.bfd /usr/bin/ld 2>/dev/null || true
        fi
        ;;
    *)
        echo "Warning: Unknown SILEX_LINKER value '$LINKER'. Using default (mold)." >&2
        export LDFLAGS="-fuse-ld=mold"
        ;;
esac

# Build system generator configuration
GENERATOR="${SILEX_GENERATOR:-Ninja}"
export CMAKE_GENERATOR="$GENERATOR"

# Parallel build configuration
PARALLEL="${SILEX_PARALLEL:-auto}"
if [ "$PARALLEL" = "auto" ]; then
    # Auto-detect CPU count
    NPROC=$(nproc 2>/dev/null || echo 4)
    export MAKEFLAGS="-j$NPROC"
elif [ "$PARALLEL" = "off" ]; then
    export MAKEFLAGS="-j1"
elif [[ "$PARALLEL" =~ ^[0-9]+$ ]]; then
    export MAKEFLAGS="-j$PARALLEL"
else
    echo "Warning: Invalid SILEX_PARALLEL value '$PARALLEL'. Using auto." >&2
    NPROC=$(nproc 2>/dev/null || echo 4)
    export MAKEFLAGS="-j$NPROC"
fi

# Compiler cache configuration
CACHE="${SILEX_CACHE:-sccache}"
case "$CACHE" in
    sccache)
        export RUSTC_WRAPPER=sccache
        export CMAKE_C_COMPILER_LAUNCHER=sccache
        export CMAKE_CXX_COMPILER_LAUNCHER=sccache
        ;;
    ccache)
        export CMAKE_C_COMPILER_LAUNCHER=ccache
        export CMAKE_CXX_COMPILER_LAUNCHER=ccache
        unset RUSTC_WRAPPER
        ;;
    off)
        unset RUSTC_WRAPPER
        unset CMAKE_C_COMPILER_LAUNCHER
        unset CMAKE_CXX_COMPILER_LAUNCHER
        ;;
    *)
        echo "Warning: Unknown SILEX_CACHE value '$CACHE'. Using sccache." >&2
        export RUSTC_WRAPPER=sccache
        export CMAKE_C_COMPILER_LAUNCHER=sccache
        export CMAKE_CXX_COMPILER_LAUNCHER=sccache
        ;;
esac

# Compression configuration
COMPRESSOR="${SILEX_COMPRESSOR:-zstd}"
case "$COMPRESSOR" in
    zstd)
        export ZSTD_CLEVEL=3
        export ZSTD_NBTHREADS=0  # 0 = auto-detect
        ;;
    lz4)
        # lz4 doesn't have many env vars to configure
        :
        ;;
    gzip)
        # Use pigz if available for parallel gzip
        if command -v pigz >/dev/null 2>&1; then
            alias gzip=pigz
        fi
        ;;
    *)
        echo "Warning: Unknown SILEX_COMPRESSOR value '$COMPRESSOR'. Using zstd." >&2
        export ZSTD_CLEVEL=3
        export ZSTD_NBTHREADS=0
        ;;
esac

# ============================================================================
# DNS cache refresh
# ============================================================================
# Remove stale silex-dns-cache entries from previous image bake, re-resolve.
# Runs in background so it never blocks startup. Falls back to normal DNS on miss.
_silex_refresh_dns() {
    sed -i '/# silex-dns-cache$/d' /etc/hosts 2>/dev/null || return
    for _h in packages.wolfi.dev github.com objects.githubusercontent.com \
               pypi.org registry.npmjs.org crates.io proxy.golang.org; do
        _ip=$(getent ahosts "$_h" 2>/dev/null | awk '/STREAM/{print $1;exit}')
        [ -n "$_ip" ] && echo "$_ip $_h  # silex-dns-cache" >> /etc/hosts
    done
} 2>/dev/null &

# ============================================================================
# Print configuration summary (unless SILEX_QUIET=on)
# ============================================================================
QUIET="${SILEX_QUIET:-off}"
if [ "$QUIET" != "on" ]; then
    _malloc_label="${SILEX_MALLOC:-jemalloc}"
    _wrappers_label="${SILEX_WRAPPERS:-on}"
    echo "Silex: CC=$CC CXX=$CXX linker=$LINKER generator=$GENERATOR parallel=${PARALLEL} cache=$CACHE malloc=$_malloc_label wrappers=$_wrappers_label" >&2
fi

# ============================================================================
# Execute user command
# ============================================================================
# If no command is provided, start an interactive shell
if [ $# -eq 0 ]; then
    exec /bin/bash
else
    exec "$@"
fi
