FROM debian:bookworm-slim

# Install all dependencies for external test suites
RUN apt-get update && apt-get install -y \
    # Build essentials
    gcc make linux-libc-dev \
    # Version control and network
    git curl wget ca-certificates \
    # Python (for Oils/OSH)
    python3 python3-pip \
    # GNU autotools dependencies (with gnulib)
    autopoint gperf gettext texinfo gnulib \
    # Smoosh (OCaml-based)
    ocaml opam \
    # mksh (expect-based tests)
    expect tcl \
    # Build tools for GNU test suites
    automake autoconf libtool pkg-config bison flex \
    # Additional utilities
    perl cpanminus rsync \
    # Shell utilities
    dash bash \
    && rm -rf /var/lib/apt/lists/*

# Configure git to trust all directories (needed for external repos)
RUN git config --global --add safe.directory '*'

WORKDIR /silex

# Copy entire source tree
COPY . .

# Set environment
ENV SILEX=/silex/build/bin/silex

CMD ["/bin/bash"]
