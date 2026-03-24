# Tests: nodejs, npm, node-gyp C++ native module compilation
# Original: FROM ubuntu:24.04

FROM silex:slim

RUN apt-get update && apt-get install -y \
    nodejs \
    npm \
    build-essential \
    python3

WORKDIR /tmp/sharp-test
RUN npm install sharp@0.33.5

RUN node -e "const s = require('sharp'); console.log('sharp ok, vips', s.versions.vips)"
