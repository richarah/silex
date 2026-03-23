FROM ghcr.io/richarah/silex:slim

# Install Bazel via Bazelisk (version manager)
RUN curl -fsSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
    -o /usr/local/bin/bazel && \
    chmod +x /usr/local/bin/bazel

WORKDIR /workspace
COPY . .

# Bazel disk cache keeps build artifacts across Docker builds.
# Silex's mold and clang are picked up automatically via the host PATH.
RUN --mount=type=cache,target=/root/.cache/bazel \
    bazel build //... \
        --disk_cache=/root/.cache/bazel \
        --action_env=CC=clang \
        --action_env=CXX=clang++

CMD ["./bazel-bin/my-app/my-app"]
