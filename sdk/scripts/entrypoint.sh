#!/bin/sh
# Silex entrypoint: reads SILEX_* environment variables and configures build environment.
# Called by tini as the container entrypoint.
# POSIX sh only — no bash, no [[, no arrays.

# ============================================================================
# Allocator configuration (must be first — sets LD_PRELOAD before anything else)
# ============================================================================
MALLOC="${SILEX_MALLOC:-mimalloc}"
case "$MALLOC" in
    mimalloc)
        _mimalloc_so=$(find /usr/lib /usr/local/lib -name 'libmimalloc.so*' 2>/dev/null | sort | tail -1)
        if [ -n "$_mimalloc_so" ]; then
            export LD_PRELOAD="$_mimalloc_so"
        else
            echo "silex: warning: mimalloc library not found, using system allocator" >&2
        fi
        ;;
    jemalloc)
        _jemalloc_so=$(find /usr/lib /usr/local/lib -name 'libjemalloc.so*' 2>/dev/null | head -1)
        if [ -n "$_jemalloc_so" ]; then
            export LD_PRELOAD="$_jemalloc_so"
        else
            echo "silex: warning: jemalloc requested but library not found, using system allocator" >&2
        fi
        ;;
    system)
        unset LD_PRELOAD
        ;;
    *)
        echo "silex: warning: unknown SILEX_MALLOC='$MALLOC', using mimalloc" >&2
        _mimalloc_so=$(find /usr/lib /usr/local/lib -name 'libmimalloc.so*' 2>/dev/null | sort | tail -1)
        [ -n "$_mimalloc_so" ] && export LD_PRELOAD="$_mimalloc_so"
        ;;
esac

# ============================================================================
# Feature flags (exported so wrapper scripts and subprocesses can read them)
# ============================================================================
export SILEX_WRAPPERS="${SILEX_WRAPPERS:-on}"
export SILEX_GIT_SHALLOW="${SILEX_GIT_SHALLOW:-on}"
export SILEX_APT_SHIM="${SILEX_APT_SHIM:-on}"

# ============================================================================
# Compiler configuration
# ============================================================================
export CC="${SILEX_CC:-clang}"
export CXX="${SILEX_CXX:-clang++}"

# ============================================================================
# Linker configuration
# ============================================================================
LINKER="${SILEX_LINKER:-mold}"
case "$LINKER" in
    mold)
        export LDFLAGS="-fuse-ld=mold"
        [ ! -L /usr/bin/ld ] && ln -sf /opt/mold/bin/mold /usr/bin/ld 2>/dev/null || true
        ;;
    lld)
        export LDFLAGS="-fuse-ld=lld"
        command -v lld >/dev/null 2>&1 && ln -sf "$(command -v lld)" /usr/bin/ld 2>/dev/null || true
        ;;
    gold)
        export LDFLAGS="-fuse-ld=gold"
        command -v gold >/dev/null 2>&1 && ln -sf "$(command -v gold)" /usr/bin/ld 2>/dev/null || true
        ;;
    ld)
        export LDFLAGS=""
        [ -f /usr/bin/ld.bfd ] && ln -sf /usr/bin/ld.bfd /usr/bin/ld 2>/dev/null || true
        ;;
    *)
        echo "silex: warning: unknown SILEX_LINKER='$LINKER', using mold" >&2
        export LDFLAGS="-fuse-ld=mold"
        ;;
esac

# ============================================================================
# Build system generator
# ============================================================================
GENERATOR="${SILEX_GENERATOR:-Ninja}"
export CMAKE_GENERATOR="$GENERATOR"

# ============================================================================
# Parallel build configuration
# ============================================================================
PARALLEL="${SILEX_PARALLEL:-auto}"
case "$PARALLEL" in
    auto)
        NPROC=$(nproc 2>/dev/null || echo 4)
        export MAKEFLAGS="-j$NPROC"
        ;;
    off)
        export MAKEFLAGS="-j1"
        ;;
    *)
        if printf '%s' "$PARALLEL" | grep -qE '^[0-9]+$'; then
            export MAKEFLAGS="-j$PARALLEL"
        else
            echo "silex: warning: invalid SILEX_PARALLEL='$PARALLEL', using auto" >&2
            NPROC=$(nproc 2>/dev/null || echo 4)
            export MAKEFLAGS="-j$NPROC"
        fi
        ;;
esac

# ============================================================================
# Compiler cache configuration
# ============================================================================
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
        echo "silex: warning: unknown SILEX_CACHE='$CACHE', using sccache" >&2
        export RUSTC_WRAPPER=sccache
        export CMAKE_C_COMPILER_LAUNCHER=sccache
        export CMAKE_CXX_COMPILER_LAUNCHER=sccache
        ;;
esac

# ============================================================================
# DNS cache refresh
# Remove stale baked-in entries, re-resolve in background.
# ============================================================================
(
    sed -i '/# silex-dns-cache$/d' /etc/hosts 2>/dev/null || true
    for _h in packages.wolfi.dev github.com objects.githubusercontent.com \
               pypi.org registry.npmjs.org crates.io proxy.golang.org; do
        _ip=$(getent ahosts "$_h" 2>/dev/null | awk '/STREAM/{print $1;exit}')
        [ -n "$_ip" ] && echo "$_ip $_h  # silex-dns-cache" >> /etc/hosts
    done
) 2>/dev/null &

# ============================================================================
# Print configuration summary (unless SILEX_QUIET=on)
# ============================================================================
if [ "${SILEX_QUIET:-off}" != "on" ]; then
    echo "silex: CC=$CC CXX=$CXX linker=$LINKER generator=$GENERATOR parallel=$PARALLEL cache=$CACHE malloc=$MALLOC" >&2
fi

# ============================================================================
# Execute user command
# ============================================================================
if [ $# -eq 0 ]; then
    exec /bin/sh
else
    exec "$@"
fi
