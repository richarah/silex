# Host Recommendations

Settings that improve silex performance on the host.

## Docker BuildKit

BuildKit is required for cache mounts. Enable it:

```bash
export DOCKER_BUILDKIT=1
# or permanently in /etc/docker/daemon.json:
{ "features": { "buildkit": true } }
```

## BuildKit cache mounts in your Dockerfile

```dockerfile
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --build build
```

Cache mount paths:
- sccache: `/root/.cache/sccache`
- cargo: `/root/.cargo/registry`
- pip/uv: `/root/.cache/pip`, `/root/.cache/uv`
- npm: `/root/.npm`
- apt: `/var/cache/apt`

## ulimits

Linkers and compilers open many file descriptors simultaneously:

```bash
# /etc/security/limits.conf or Docker --ulimit
* soft nofile 65535
* hard nofile 65535
```

Or per-container:
```bash
docker run --ulimit nofile=65535:65535 silex:slim ...
```

## tmpfs for build directories

For CPU-bound builds where disk I/O is a bottleneck:

```bash
docker run --tmpfs /tmp:rw,size=4g silex:slim ...
```

## CPU pinning

For reproducible benchmarks (prevents scheduler noise):

```bash
docker run --cpuset-cpus="0-7" silex:slim ...
```

## cgroups v2

Ensure your Docker host uses cgroups v2. Most modern Linux distributions (Fedora, Ubuntu 22.04+) default to it. Improves resource accounting.

## apk package pre-fetch

Pre-fetch packages into a host cache before the build layer runs. Avoids re-downloading the same packages across builds:

```dockerfile
RUN --mount=type=cache,target=/var/cache/apk \
    apk add --cache-dir /var/cache/apk --no-network-cache \
    clang cmake ninja || \
    apk add --no-cache clang cmake ninja
```

Or pre-fetch outside Docker and bind-mount the cache:

```bash
# On the host: pre-populate a local apk cache
docker run --rm -v /tmp/apk-cache:/cache cgr.dev/chainguard/wolfi-base \
    apk fetch --output /cache clang ninja cmake libssl-dev

# In Dockerfile:
# RUN --mount=type=bind,source=/tmp/apk-cache,target=/pkg-cache \
#     apk add --allow-untrusted /pkg-cache/*.apk
```

For CI pipelines with BuildKit, the simplest approach is the cache mount:

```dockerfile
RUN --mount=type=cache,target=/var/cache/apk \
    apk add --no-cache your-packages-here
```

Cache mount path for apk: `/var/cache/apk`
