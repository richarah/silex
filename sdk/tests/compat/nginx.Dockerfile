# Tests: libpcre3-dev, libssl-dev, zlib1g-dev, autoconf-style configure
# Original: FROM ubuntu:24.04

FROM silex:slim

RUN apt-get update && apt-get install -y \
    build-essential \
    curl \
    libpcre3-dev \
    libssl-dev \
    zlib1g-dev

RUN curl -fsSL https://nginx.org/download/nginx-1.26.2.tar.gz \
        -o /tmp/nginx.tar.gz && \
    tar xf /tmp/nginx.tar.gz -C /tmp && \
    cd /tmp/nginx-1.26.2 && \
    ./configure --with-http_ssl_module && \
    make -j$(nproc)
