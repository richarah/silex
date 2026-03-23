# Multi-stage: build with silex:slim, run with silex:runtime (minimal, no compiler)
# silex:runtime is <30MB vs silex:slim's ~500MB.

# Build stage
FROM ghcr.io/richarah/silex:slim AS builder

WORKDIR /workspace
COPY . .

RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

# Runtime stage — no compiler, just libc + tini
FROM ghcr.io/richarah/silex:runtime

COPY --from=builder /workspace/build/my-app /usr/local/bin/

CMD ["my-app"]
