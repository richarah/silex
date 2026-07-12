#!/bin/bash
# run_tests.sh — run the silex unit tests
# Usage: run_tests.sh [path/to/silex]
#
# Suites are globbed, not listed. The list used to be hardcoded to four files,
# which meant tests/unit/test_vcs.sh and all ten tests/unit/shell/*.sh
# (expansion, control flow, traps, redirects, errexit -- 263 assertions) were
# never run by `make test` or by CI. They only ran under `make shell-test`,
# which no CI job invoked. Add a file, it runs.

set -euo pipefail

SILEX="${1:-build/bin/silex}"

# The suites cd around; a relative path stops resolving once they do.
case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;;
esac

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found at '$SILEX'" >&2
    exit 1
fi

PASS=0
FAIL=0
FAILED=""
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

run_suite() {
    local suite="$1"
    local name="${suite#"$SCRIPT_DIR/"}"
    echo "--- $name ---"
    if bash "$suite" "$SILEX"; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        FAIL=$((FAIL + 1))
        FAILED="$FAILED $name"
    fi
}

shopt -s nullglob
for suite in "$SCRIPT_DIR"/test_*.sh "$SCRIPT_DIR"/shell/test_*.sh; do
    run_suite "$suite"
done
shopt -u nullglob

if [ "$((PASS + FAIL))" -eq 0 ]; then
    echo "ERROR: no test suites found under $SCRIPT_DIR" >&2
    exit 1
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "Failing suites:$FAILED"
fi
[ "$FAIL" -eq 0 ]
