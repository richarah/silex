#!/bin/bash
# Integration test: build fmtlib/fmt inside silex:slim
# fmtlib: popular C++ formatting library, complex CMake build with tests

set -e

IMAGE="${1:-silex:slim}"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; ((PASS++)); }
fail() { echo "  [FAIL] $1"; ((FAIL++)); }

echo "Integration Test: fmtlib/fmt"
echo "Image: $IMAGE"
echo ""

OUTPUT=$(docker run --rm "$IMAGE" bash -c '
set -e
cd /tmp
git clone --depth=1 https://github.com/fmtlib/fmt.git 2>/dev/null
cd fmt
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DFMT_TEST=OFF \
    -DFMT_DOC=OFF \
    -DFMT_INSTALL=OFF \
    -Wno-dev \
    2>/dev/null
cmake --build build --parallel "$(nproc)" 2>/dev/null
# Smoke test: include and use fmt
cat > /tmp/test_fmt.cpp <<'"'"'EOF'"'"'
#include <fmt/core.h>
#include <cassert>
#include <string>
int main() {
    std::string result = fmt::format("Hello, {}! The answer is {}.", "world", 42);
    assert(result == "Hello, world! The answer is 42.");
    fmt::print("fmt smoke test: {}\n", result);
    return 0;
}
EOF
clang++ -std=c++17 -I/tmp/fmt/include /tmp/build/libfmt.a /tmp/test_fmt.cpp -o /tmp/test_fmt
/tmp/test_fmt
echo "SMOKE_TEST_PASS"
' 2>&1)

if echo "$OUTPUT" | grep -q "SMOKE_TEST_PASS"; then
    pass "fmtlib/fmt: clone, build, and smoke test"
else
    fail "fmtlib/fmt: build or smoke test failed"
    echo "  Output: $(echo "$OUTPUT" | tail -5)"
fi

echo ""
echo "Results: ${PASS} passed / ${FAIL} failed"
exit $FAIL
