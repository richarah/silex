#!/bin/sh
# tests/stress/run_stress.sh — run all test suites N times
# Usage: run_stress.sh [LOOPS]
# Any failure causes immediate exit with the failing iteration number.

set -e

LOOPS=${1:-100}
MATCHBOX="${2:-build/bin/matchbox}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found at $MATCHBOX"
    exit 1
fi

echo "Starting stress test: $LOOPS iterations"

i=1
while [ "$i" -le "$LOOPS" ]; do
    # Unit tests
    if ! bash tests/unit/run_tests.sh "$MATCHBOX" >/dev/null 2>&1; then
        echo "FAIL: unit tests on iteration $i"
        exit 1
    fi

    # Integration tests
    if ! bash tests/integration/run_integration.sh "$MATCHBOX" >/dev/null 2>&1; then
        echo "FAIL: integration tests on iteration $i"
        exit 1
    fi

    # Print progress every 10 iterations
    if [ $(( i % 10 )) -eq 0 ]; then
        echo "  iteration $i/$LOOPS OK"
    fi

    i=$(( i + 1 ))
done

echo "PASS: $LOOPS stress iterations"
