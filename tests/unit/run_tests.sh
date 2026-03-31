#!/bin/bash
# run_tests.sh — run all silex Phase 1 unit tests
# Usage: run_tests.sh [path/to/silex]

set -euo pipefail

SILEX="${1:-build/bin/silex}"

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found at '$SILEX'" >&2
    exit 1
fi

PASS=0
FAIL=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

run_suite() {
    local suite="$1"
    echo "--- $suite ---"
    if bash "$SCRIPT_DIR/$suite" "$SILEX"; then
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
