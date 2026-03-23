FROM ghcr.io/richarah/silex:slim

WORKDIR /workspace
COPY . .

# clang, mold, Ninja, and sccache are pre-configured.
# Use BuildKit cache mounts for sccache across builds:
#   docker build --build-arg BUILDKIT_INLINE_CACHE=1 .
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

CMD ["./build/my-app"]
