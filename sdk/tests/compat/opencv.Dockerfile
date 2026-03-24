# Tests: cmake, pkg-config, libavcodec/format/swscale-dev (ffmpeg-dev),
#        python3-dev, libtbb-dev, libjpeg-dev, libpng-dev, libtiff-dev
#        libgtk2.0-dev intentionally included (shim skips it; cmake auto-detects)
#        -DWITH_FFMPEG=OFF: Wolfi ffmpeg is newer than OpenCV 4.10.0 expects;
#        avcodec_close/av_stream_get_side_data removed in ffmpeg 6+.
# Original: FROM ubuntu:24.04
# Headline benchmark test. Slow on Ubuntu, fast on silex.

FROM silex:slim

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libavcodec-dev \
    libavformat-dev \
    libswscale-dev \
    python3-dev \
    libtbb-dev \
    libjpeg-dev \
    libpng-dev \
    libtiff-dev \
    libgtk2.0-dev

RUN curl -fsSL https://github.com/opencv/opencv/archive/4.10.0.tar.gz \
        -o /tmp/opencv.tar.gz && \
    tar xf /tmp/opencv.tar.gz -C /tmp && \
    cmake -B /tmp/opencv-build -S /tmp/opencv-4.10.0 \
        -DWITH_GTK=OFF \
        -DWITH_FFMPEG=OFF \
        -DBUILD_TESTS=OFF \
        -DBUILD_PERF_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF && \
    cmake --build /tmp/opencv-build -j$(nproc)
