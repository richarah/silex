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

# ----------------------------------------------------------------------------
# sccache backend.
#
# The README's headline is "18x warm sccache rebuild" -- and that number is only
# real if the cache SURVIVES between builds. The default backend is a local
# directory (SCCACHE_DIR), which persists on one developer's machine via a
# BuildKit `--mount=type=cache`, but is EMPTY on every fresh CI runner. So the
# advertised win never materialised in the one place people most want it: CI.
#
# sccache reads its backend from environment variables, and it prefers, in order:
# GitHub Actions cache, S3/R2, Redis, memcached, then the local dir. We do not
# hardcode credentials -- we just make the common backends work when the caller
# supplies them, and say which one is active.
#
#   GitHub Actions:  set SILEX_SCCACHE=gha and pass ACTIONS_CACHE_URL +
#                    ACTIONS_RUNTIME_TOKEN (the standard `docker/build-push` +
#                    `crazy-max/ghaction-github-runtime` combination).
#   S3 / R2 / MinIO: set SILEX_SCCACHE=s3 and the SCCACHE_BUCKET / SCCACHE_ENDPOINT
#                    / AWS_* variables sccache already understands.
#   local (default): a directory, persisted across builds only by a cache mount.
#
# This is opt-in: with nothing set, behaviour is exactly as before.
if [ "$CACHE" = "sccache" ]; then
    case "${SILEX_SCCACHE:-local}" in
        gha)
            if [ -n "${ACTIONS_CACHE_URL:-}" ] && [ -n "${ACTIONS_RUNTIME_TOKEN:-}" ]; then
                export SCCACHE_GHA_ENABLED=true
                unset SCCACHE_DIR
                [ "${SILEX_QUIET:-off}" = on ] || echo "silex: sccache -> GitHub Actions cache" >&2
            else
                echo "silex: SILEX_SCCACHE=gha but ACTIONS_CACHE_URL/ACTIONS_RUNTIME_TOKEN are unset;" >&2
                echo "       falling back to the local cache dir. In a workflow, add" >&2
                echo "       'uses: crazy-max/ghaction-github-runtime@v3' to export them." >&2
            fi
            ;;
        s3)
            if [ -n "${SCCACHE_BUCKET:-}" ]; then
                unset SCCACHE_DIR
                [ "${SILEX_QUIET:-off}" = on ] || echo "silex: sccache -> S3 bucket $SCCACHE_BUCKET" >&2
            else
                echo "silex: SILEX_SCCACHE=s3 but SCCACHE_BUCKET is unset; using the local cache dir." >&2
            fi
            ;;
        local|"")
            : # SCCACHE_DIR as set in the Dockerfile
            ;;
        *)
            echo "silex: warning: unknown SILEX_SCCACHE='$SILEX_SCCACHE'; using the local cache dir." >&2
            ;;
    esac
fi

# DNS cache refresh was here. Removed.
#
# It re-resolved seven hosts and rewrote /etc/hosts from a DETACHED BACKGROUND
# SUBSHELL on every single container start -- racing the user's command, with no
# synchronisation. A fast `RUN` could read /etc/hosts while it was half-written.
#
# And the addresses it cached are Fastly/GitHub anycast IPs, which rotate. A
# stale pin does not make the build faster; it sends package installs to whoever
# holds that address next. The resolver already caches. Let it.

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
