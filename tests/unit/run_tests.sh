#!/bin/bash
# run_tests.sh — run all matchbox Phase 1 unit tests
# Usage: run_tests.sh [path/to/matchbox]

set -euo pipefail

MATCHBOX="${1:-build/bin/matchbox}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found at '$MATCHBOX'" >&2
    exit 1
fi

PASS=0
FAIL=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

run_suite() {
    local suite="$1"
    echo "--- $suite ---"
    if bash "$SCRIPT_DIR/$suite" "$MATCHBOX"; then
        echo "PASS: $suite"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $suite"
        FAIL=$((FAIL + 1))
    fi
}

run_suite test_echo.sh
run_suite test_mkdir.sh
run_suite test_cp.sh
run_suite test_shell_builtins.sh

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
