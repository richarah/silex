FROM ghcr.io/richarah/silex:slim

# Install Conan C++ package manager
RUN pip install --no-cache-dir conan

WORKDIR /workspace
COPY conanfile.txt CMakeLists.txt ./

# Install Conan dependencies (cached across builds)
RUN --mount=type=cache,target=/root/.conan2 \
    conan install . --build=missing \
        -s compiler=clang \
        -s compiler.version=18 \
        -s compiler.libcxx=libc++

COPY src ./src

# Build with CMake + Ninja (Silex defaults)
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake && \
    cmake --build build

CMD ["./build/my-app"]
