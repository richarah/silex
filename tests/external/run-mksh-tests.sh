#!/bin/sh
# run-mksh-tests.sh — Run mksh test suite against silex
# Suite 4: MirBSD Korn Shell regression tests
#
# mksh is a mature, production Korn Shell implementation. Its test suite
# (check.t) covers shell features, builtins, and edge cases.
#
# Expected pass rate: Moderate (silex is POSIX sh, not ksh)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
MKSH_DIR="$REPOS_DIR/mksh"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== mksh Test Suite ==="
echo "Suite: MirBSD Korn Shell regression tests"
echo "Binary: $SILEX"
echo ""

# Verify mksh repo exists
if [ ! -d "$MKSH_DIR" ]; then
    echo "ERROR: mksh repo not found at $MKSH_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$MKSH_DIR"

# Check if check.t and check.pl exist
if [ ! -f "check.t" ]; then
    echo "ERROR: mksh test suite (check.t) not found"
    echo "mksh: pass=0 fail=0 total=0"
    exit 1
fi

if [ ! -f "check.pl" ]; then
    echo "ERROR: mksh test runner (check.pl) not found"
    echo "mksh: pass=0 fail=0 total=0"
    exit 1
fi

# Check for perl
if ! command -v perl >/dev/null 2>&1; then
    echo "WARNING: perl not found (required by check.pl)"
    echo "Skipping mksh tests."
    echo ""
    echo "mksh: pass=0 fail=0 total=0"
    exit 0
fi

echo "Running mksh test suite with silex..."
echo "(This may take 5-10 minutes - 627 tests)"
echo ""

# Run check.pl with silex as the test shell
# -p: program to test, -s: test set file
TEST_OUTPUT=$(perl check.pl -p "$SILEX" -s check.t 2>&1 || true)

# Show last 100 lines of output
echo "$TEST_OUTPUT" | tail -100

# Parse results from output
# check.pl output format varies, try multiple patterns
PASS=$(echo "$TEST_OUTPUT" | grep -oE '[0-9]+ passed' | head -1 | grep -oE '[0-9]+' || echo 0)
FAIL=$(echo "$TEST_OUTPUT" | grep -oE '[0-9]+ failed' | head -1 | grep -oE '[0-9]+' || echo 0)

# Try alternate formats if first didn't work
if [ "$PASS" -eq 0 ] && [ "$FAIL" -eq 0 ]; then
    PASS=$(echo "$TEST_OUTPUT" | grep -oE 'passed: [0-9]+' | grep -oE '[0-9]+' | head -1 || echo 0)
    FAIL=$(echo "$TEST_OUTPUT" | grep -oE 'failed: [0-9]+' | grep -oE '[0-9]+' | head -1 || echo 0)
fi

# Try summary line format
if [ "$PASS" -eq 0 ] && [ "$FAIL" -eq 0 ]; then
    # Format: "Total: 627 passed, 0 failed"
    PASS=$(echo "$TEST_OUTPUT" | grep -i total | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+' | head -1 || echo 0)
    FAIL=$(echo "$TEST_OUTPUT" | grep -i total | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+' | head -1 || echo 0)
fi

TOTAL=$((PASS + FAIL))

# Fallback: count test cases if parsing still failed
if [ "$TOTAL" -eq 0 ]; then
    echo ""
    echo "WARNING: Could not parse test results from check.pl output"
    echo "Counting test cases in check.t as fallback"
    TOTAL=$(grep -c "^name:" check.t 2>/dev/null || echo 627)
    PASS=0
    FAIL=0
fi

echo ""
echo "=== mksh Test Results ==="
echo ""

echo "  TOTAL: $TOTAL"
echo "  PASS:  $PASS"
echo "  FAIL:  $FAIL"
echo ""

echo "mksh: pass=$PASS fail=$FAIL total=$TOTAL"

echo ""
echo "Result: Tests completed"
echo ""
echo "Note: Many failures expected - silex is POSIX sh, not ksh."
echo ""

exit 0
