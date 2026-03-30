#!/bin/sh
# run_all.sh — Master conformance test runner
# Runs all conformance test scripts and reports a summary.

MATCHBOX="${MATCHBOX:-$(dirname "$0")/../../build/bin/matchbox}"
PASS=0; FAIL=0

echo "=== matchbox conformance suite ==="
echo "Binary: $MATCHBOX"
echo ""

run_suite() {
    local name="$1" script="$2"
    echo "--- $name ---"
    if sh "$script" 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    echo ""
}

DIR="$(dirname "$0")"
run_suite "Quoting/Expansion (Q-01..Q-08)" "$DIR/quoting_test.sh"
run_suite "GNU Tool Comparison (GNU-01)"   "$DIR/gnu_compare.sh"

printf '\nOverall: %d suites passed, %d suites failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
