# Tests: libreadline-dev, libssl-dev, zlib1g-dev, libxml2-dev, flex, bison
# Original: FROM ubuntu:24.04

FROM silex:slim

RUN apt-get update && apt-get install -y \
    build-essential \
    curl \
    libreadline-dev \
    libssl-dev \
    zlib1g-dev \
    libxml2-dev \
    flex \
    bison

RUN curl -fsSL https://ftp.postgresql.org/pub/source/v16.6/postgresql-16.6.tar.gz \
        -o /tmp/pg.tar.gz && \
    tar xf /tmp/pg.tar.gz -C /tmp && \
    cd /tmp/postgresql-16.6 && \
    ./configure --without-icu --with-openssl --with-libxml && \
    make -j$(nproc)
