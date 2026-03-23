#!/bin/bash
# Integration test: build google/re2 inside silex:slim
# re2: Google's fast regular expression library, CMake + Abseil dependencies

set -e

IMAGE="${1:-silex:slim}"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; ((PASS++)); }
fail() { echo "  [FAIL] $1"; ((FAIL++)); }

echo "Integration Test: google/re2"
echo "Image: $IMAGE"
echo ""

OUTPUT=$(docker run --rm "$IMAGE" bash -c '
set -e
cd /tmp
git clone --depth=1 https://github.com/google/re2.git 2>/dev/null
cd re2
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DRE2_BUILD_TESTING=OFF \
    -Wno-dev \
    2>/dev/null
cmake --build build --parallel "$(nproc)" 2>/dev/null
# Smoke test: link against re2 and run a simple match
cat > /tmp/test_re2.cpp <<'"'"'EOF'"'"'
#include <re2/re2.h>
#include <cassert>
#include <string>
int main() {
    std::string input = "hello world 42";
    std::string number;
    assert(RE2::PartialMatch(input, R"(\d+)", &number));
    assert(number == "42");
    return 0;
}
EOF
clang++ -std=c++17 -I/tmp/re2 \
    /tmp/test_re2.cpp \
    /tmp/build/libre2.a \
    -lpthread -o /tmp/test_re2
/tmp/test_re2
echo "SMOKE_TEST_PASS"
' 2>&1)

if echo "$OUTPUT" | grep -q "SMOKE_TEST_PASS"; then
    pass "google/re2: clone, build, and smoke test"
else
    fail "google/re2: build or smoke test failed"
    echo "  Output: $(echo "$OUTPUT" | tail -5)"
fi

echo ""
echo "Results: ${PASS} passed / ${FAIL} failed"
exit $FAIL
