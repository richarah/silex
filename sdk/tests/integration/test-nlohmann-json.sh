#!/bin/bash
# Integration test: build nlohmann/json inside silex:slim
# nlohmann/json: header-only C++ JSON library, widely used, CMake build system
# Tests: clone, build, run tests

set -e

IMAGE="${1:-silex:slim}"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; ((PASS++)); }
fail() { echo "  [FAIL] $1"; ((FAIL++)); }

echo "Integration Test: nlohmann/json"
echo "Image: $IMAGE"
echo ""

# Run the build inside the container
OUTPUT=$(docker run --rm "$IMAGE" bash -c '
set -e
cd /tmp
git clone --depth=1 https://github.com/nlohmann/json.git 2>/dev/null
cd json
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DJSON_BuildTests=OFF \
    -DJSON_CI=OFF \
    -DCMAKE_VERBOSE_MAKEFILE=OFF \
    -Wno-dev \
    2>/dev/null
cmake --build build --parallel "$(nproc)" 2>/dev/null
# Run a quick smoke test
cat > /tmp/test_json.cpp <<'"'"'EOF'"'"'
#include <nlohmann/json.hpp>
#include <cassert>
int main() {
    auto j = nlohmann::json::parse(R"({"key": "value", "num": 42})");
    assert(j["key"] == "value");
    assert(j["num"] == 42);
    return 0;
}
EOF
clang++ -std=c++17 -I/tmp/json/include /tmp/test_json.cpp -o /tmp/test_json
/tmp/test_json
echo "SMOKE_TEST_PASS"
' 2>&1)

if echo "$OUTPUT" | grep -q "SMOKE_TEST_PASS"; then
    pass "nlohmann/json: clone, build, and smoke test"
else
    fail "nlohmann/json: build or smoke test failed"
    echo "  Output: $(echo "$OUTPUT" | tail -5)"
fi

echo ""
echo "Results: ${PASS} passed / ${FAIL} failed"
exit $FAIL
