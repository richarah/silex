# Cross-compile from x86_64 to arm64 using silex:cross (Dockerfile.cross).
# Requires: docker buildx with QEMU or native arm64 runner.
#
# Build:
#   docker buildx build --platform linux/amd64,linux/arm64 -t my-app .

FROM ghcr.io/richarah/silex:cross AS builder

WORKDIR /workspace
COPY . .

# clang's --target flag selects the cross-compilation target automatically.
# CMake toolchain file configures sysroot and target triple.
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/silex/toolchains/aarch64-linux-gnu.cmake && \
    cmake --build build

FROM ghcr.io/richarah/silex:runtime
COPY --from=builder /workspace/build/my-app /usr/local/bin/
CMD ["my-app"]
