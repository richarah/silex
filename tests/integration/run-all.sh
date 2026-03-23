#!/bin/bash
# Run all integration tests
# Usage: ./tests/integration/run-all.sh [image]
# Default image: silex:slim

IMAGE="${1:-silex:slim}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TOTAL_PASS=0
TOTAL_FAIL=0

for test_script in "$SCRIPT_DIR"/test-*.sh; do
    echo "================================================================"
    bash "$test_script" "$IMAGE"
    EXIT=$?
    if [ $EXIT -eq 0 ]; then
        ((TOTAL_PASS++))
    else
        ((TOTAL_FAIL++))
    fi
    echo ""
done

echo "================================================================"
echo "Integration Test Summary"
echo "  Passed: $TOTAL_PASS"
echo "  Failed: $TOTAL_FAIL"

if [ $TOTAL_FAIL -gt 0 ]; then
    exit 1
fi
