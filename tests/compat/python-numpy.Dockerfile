# Tests: python3-dev, gfortran, C extension build via pip/uv
# Original: FROM ubuntu:24.04

FROM silex:slim

RUN apt-get update && apt-get install -y \
    python3-dev \
    build-essential \
    gfortran

RUN uv pip install --system numpy

RUN python3 -c "import numpy; print(numpy.__version__)"
