# Tests: build-essential, tcl, make, basic C toolchain
# Original: FROM ubuntu:24.04
# Canary test — if this fails, something fundamental is broken.

FROM silex:slim

RUN apt-get update && apt-get install -y build-essential tcl

RUN curl -fsSL https://download.redis.io/releases/redis-7.2.7.tar.gz \
        -o /tmp/redis.tar.gz && \
    tar xf /tmp/redis.tar.gz -C /tmp && \
    cd /tmp/redis-7.2.7 && \
    make -j$(nproc)
