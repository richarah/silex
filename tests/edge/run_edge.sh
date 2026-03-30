#!/bin/sh
# tests/edge/run_edge.sh — master runner for edge case tests
# Usage: run_edge.sh [path/to/matchbox]

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0
TOTAL_PASS=0
TOTAL_FAIL=0

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found at $MATCHBOX"
    exit 1
fi

SCRIPTDIR=$(dirname "$0")

run_suite() {
    local script="$1"
    echo "--- $(basename $script) ---"
    if bash "$script" "$MATCHBOX"; then
        TOTAL_PASS=$(( TOTAL_PASS + 1 ))
    else
        TOTAL_FAIL=$(( TOTAL_FAIL + 1 ))
    fi
}

run_suite "$SCRIPTDIR/test_filenames.sh"
run_suite "$SCRIPTDIR/test_empty_files.sh"
run_suite "$SCRIPTDIR/test_long_lines.sh"
run_suite "$SCRIPTDIR/test_binary.sh"
run_suite "$SCRIPTDIR/test_limits.sh"
run_suite "$SCRIPTDIR/test_symlinks.sh"
run_suite "$SCRIPTDIR/test_io.sh"

echo ""
echo "=== Edge test suites: $TOTAL_PASS passed, $TOTAL_FAIL failed ==="
[ "$TOTAL_FAIL" -eq 0 ]
