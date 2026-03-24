# Tests: heavy package mapping coverage — libssl-dev, zlib1g-dev, libbz2-dev,
#        libreadline-dev, libsqlite3-dev, libffi-dev, liblzma-dev, libncurses-dev
# Original: FROM ubuntu:24.04
# Heaviest shim test. If this passes, most package mappings work.

FROM silex:slim

RUN apt-get update && apt-get install -y \
    build-essential \
    libssl-dev \
    zlib1g-dev \
    libbz2-dev \
    libreadline-dev \
    libsqlite3-dev \
    libffi-dev \
    liblzma-dev \
    libncurses-dev

RUN curl -fsSL https://www.python.org/ftp/python/3.12.7/Python-3.12.7.tar.xz \
        -o /tmp/cpython.tar.xz && \
    tar xf /tmp/cpython.tar.xz -C /tmp && \
    cd /tmp/Python-3.12.7 && \
    ./configure && \
    make -j$(nproc)
