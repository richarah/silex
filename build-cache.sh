#!/bin/sh
# build-cache.sh — build the silex-cache companion image.
# Usage: sh build-cache.sh [VERSION]
#
# The cache image contains:
#   - A snapshot of the Wolfi APKINDEX at build time (frozen)
#   - Pre-downloaded .apk files for commonly installed packages
#
# silex-cache is used by mounting it as a volume or COPY --from source
# to enable reproducible, offline-capable builds:
#
#   FROM silex:slim
#   COPY --from=silex-cache:VERSION /cache /var/cache/apk
#   RUN apt-get install -y build-essential  # uses cached packages
#
# Without silex-cache, apk always fetches live from apk.cgr.dev/chainguard.

set -e

VERSION="${1:-latest}"
IMAGE="silex-cache:${VERSION}"

WORKDIR=$(mktemp -d /tmp/silex-cache-XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

echo "Building ${IMAGE} ..."

# Run silex:slim to fetch the current APKINDEX and a core package set.
# The container fetches packages and writes them to /cache, then exits.
docker run --rm \
    -v "$WORKDIR:/cache" \
    silex:slim \
    sh -c '
        set -e
        # Fetch and freeze the APKINDEX.
        apk update --cache-dir /cache
        # Pre-download common build packages into the cache.
        apk fetch --cache-dir /cache --no-progress \
            build-essential \
            cmake \
            ninja-build \
            python3 \
            python3-dev \
            git \
            pkg-config \
            libssl-dev \
            zlib-dev \
            curl \
            ca-certificates \
            2>/dev/null || true
        echo "Cache populated: $(find /cache -name "*.apk" | wc -l) packages"
    '

echo "Cached $(find "$WORKDIR" -name "*.apk" | wc -l) .apk files."
echo "Building Docker image ${IMAGE} ..."

# Build the cache image: scratch with the populated cache directory.
docker build \
    --no-cache \
    -t "$IMAGE" \
    -f - "$WORKDIR" <<'DOCKERFILE'
FROM scratch
COPY . /cache
LABEL maintainer="richarah@github"
LABEL org.opencontainers.image.title="Silex Cache"
LABEL org.opencontainers.image.description="Frozen Wolfi APKINDEX + pre-downloaded packages for silex:slim."
DOCKERFILE

SIZE=$(docker image inspect "$IMAGE" --format '{{.Size}}' | awk '{printf "%.1fMB", $1/1024/1024}')
echo "${IMAGE} built (${SIZE})."
echo ""
echo "Usage in Dockerfile:"
echo "  COPY --from=${IMAGE} /cache /var/cache/apk"
echo "  RUN apt-get install -y build-essential"
