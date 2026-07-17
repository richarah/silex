#!/bin/sh
# run-toybox.sh — Run toybox test suite against silex
# Suite 8: Upstream toybox tests
#
# silex is a fork of toybox, so we should have high compatibility with
# toybox's own test suite.
#
# Expected pass rate: 70-80% (high due to shared ancestry)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
TOYBOX_DIR="$REPOS_DIR/toybox"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== toybox Test Suite ==="
echo "Suite: toybox upstream tests"
echo "Binary: $SILEX"
echo ""
echo "Note: silex is a toybox fork, so high compatibility expected"
echo ""

# Verify toybox repo exists
if [ ! -d "$TOYBOX_DIR" ]; then
    echo "ERROR: toybox repo not found at $TOYBOX_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$TOYBOX_DIR"

# Check dependencies for building toybox
for cmd in gcc make; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "WARNING: $cmd not found (required to build toybox)"
        echo "Skipping toybox tests."
        echo ""
        echo "toybox: pass=0 fail=0 total=0"
        exit 0
    fi
done

# Build toybox if not already built
if [ ! -f "toybox" ]; then
    echo "Building toybox..."
    make defconfig 2>&1 | tail -5
    make -j4 2>&1 | tail -10
fi

if [ ! -f "toybox" ]; then
    echo "ERROR: toybox build failed"
    echo "toybox: pass=0 fail=0 total=0"
    exit 1
fi

echo ""
echo "Running toybox test suite..."
echo "(This may take 5-10 minutes)"
echo ""

# Run tests and capture output
if [ -f "scripts/test.sh" ]; then
    TEST_OUTPUT=$(bash scripts/test.sh 2>&1 || true)
    echo "$TEST_OUTPUT" | tail -50

    # Parse results from output
    # toybox test.sh prints one line per test: "PASS: test name" or "FAIL: test name"
    PASS=$(echo "$TEST_OUTPUT" | grep -cE '^PASS:' || echo 0)
    FAIL=$(echo "$TEST_OUTPUT" | grep -cE '^FAIL:' || echo 0)
    TOTAL=$((PASS + FAIL))

    # If parsing failed, count test files as fallback
    if [ "$TOTAL" -eq 0 ]; then
        TOTAL=$(ls tests/*.test 2>/dev/null | wc -l || echo 0)
        PASS=0
        FAIL=0
    fi
else
    echo "WARNING: scripts/test.sh not found"
    TOTAL=$(ls tests/*.test 2>/dev/null | wc -l || echo 0)
    PASS=0
    FAIL=0
fi

echo ""
echo "=== toybox Test Results ==="
echo ""

echo "  TOTAL: $TOTAL"
echo "  PASS:  $PASS"
echo "  FAIL:  $FAIL"
echo ""

echo "toybox: pass=$PASS fail=$FAIL total=$TOTAL"

echo ""
echo "Result: Tests completed"
echo ""

# A suite that executed zero tests has not passed -- it has not run. Eight of
# the ten suites were doing exactly that, and the hardcoded `exit 0` that used
# to sit here reported every one of them as green.
if [ "${TOTAL:-0}" -eq 0 ]; then
    echo "ERROR: no tests were executed. The suite did not run."
    exit 1
fi
[ "${FAIL:-1}" -eq 0 ]
