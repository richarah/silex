#!/bin/sh
# run-smoosh.sh — Run Smoosh tests against silex
# Suite 2: 157 tests derived from formal mechanized semantics
#
# Smoosh is a POSIX shell formalized in Coq (mechanized formal semantics).
# These tests are derived from the formal model and represent the "gold standard"
# for POSIX shell behavior - if Smoosh says it's correct, it IS correct.
#
# Expected pass rate: High (these are unambiguous POSIX tests)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
SMOOSH_DIR="$REPOS_DIR/smoosh"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== Smoosh Formal Semantics Tests ==="
echo "Suite: Smoosh (mechanized POSIX shell semantics)"
echo "Tests: 157 tests from formal Coq model"
echo "Binary: $SILEX"
echo ""

# Verify Smoosh repo exists
if [ ! -d "$SMOOSH_DIR" ]; then
    echo "ERROR: Smoosh repo not found at $SMOOSH_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$SMOOSH_DIR"

# Check if Smoosh test directory exists
if [ ! -d "tests/shell" ]; then
    echo "ERROR: Smoosh tests/shell directory not found"
    echo "The Smoosh repo structure may have changed."
    exit 1
fi

# Technique: Run .test files directly as shell scripts, compare stdout/stderr
# Each test has: .test (script), .out (expected stdout), .err (expected stderr)

echo "Running Smoosh tests with silex..."
echo "(This may take 2-5 minutes)"
echo ""

PASS=0
FAIL=0
TOTAL=0

cd tests/shell

# Run each .test file with timeout to prevent hangs
for test_file in *.test; do
    [ ! -f "$test_file" ] && continue

    TOTAL=$((TOTAL + 1))
    base="${test_file%.test}"

    # Progress indicator (every 20 tests)
    if [ $((TOTAL % 20)) -eq 0 ]; then
        echo "  Progress: $TOTAL tests..."
    fi

    # Run test with 5-second timeout
    # Use timeout command if available, otherwise run without it
    if command -v timeout >/dev/null 2>&1; then
        timeout 5 "$SILEX" "$test_file" >"/tmp/smoosh-out-$$" 2>"/tmp/smoosh-err-$$" || true
    else
        "$SILEX" "$test_file" >"/tmp/smoosh-out-$$" 2>"/tmp/smoosh-err-$$" || true
    fi

    # Compare output
    out_ok=1
    err_ok=1

    if [ -f "$base.out" ]; then
        if ! diff -q "$base.out" "/tmp/smoosh-out-$$" >/dev/null 2>&1; then
            out_ok=0
        fi
    fi

    if [ -f "$base.err" ]; then
        if ! diff -q "$base.err" "/tmp/smoosh-err-$$" >/dev/null 2>&1; then
            err_ok=0
        fi
    fi

    if [ $out_ok -eq 1 ] && [ $err_ok -eq 1 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi

    rm -f "/tmp/smoosh-out-$$" "/tmp/smoosh-err-$$"
done

echo ""
echo "=== Smoosh Test Results ==="
echo ""

echo "  TOTAL: $TOTAL"
echo "  PASS:  $PASS"
echo "  FAIL:  $FAIL"
echo ""

echo "Smoosh: pass=$PASS fail=$FAIL total=$TOTAL"

echo ""
echo "Result: Tests completed"
echo ""

exit 0
