FROM ghcr.io/richarah/silex:slim

# Install Rust toolchain
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
ENV PATH="/root/.cargo/bin:$PATH"

WORKDIR /workspace
COPY . .

# RUSTC_WRAPPER=sccache is already set by silex entrypoint.
# Cargo registry cache is mounted for faster repeated builds.
RUN --mount=type=cache,target=/root/.cargo/registry \
    --mount=type=cache,target=/root/.cache/sccache \
    cargo build --release

CMD ["./target/release/my-app"]
