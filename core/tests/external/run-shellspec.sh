#!/bin/sh
# run-shellspec.sh — Run ShellSpec against silex (meta-test)
# Suite 9: ShellSpec BDD testing framework
#
# ShellSpec is a BDD (Behavior-Driven Development) testing framework written
# entirely in POSIX shell. If ShellSpec can run on silex, it validates that
# silex implements enough shell features to run complex shell programs.
#
# Expected: High pass rate (90%+) or complete failure (binary outcome)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
SHELLSPEC_DIR="$REPOS_DIR/shellspec"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== ShellSpec Meta-Test ==="
echo "Suite: ShellSpec (BDD framework in POSIX sh)"
echo "Binary: $SILEX"
echo ""
echo "Meta-test: If ShellSpec runs, it validates silex shell implementation"
echo ""

# Verify ShellSpec repo exists
if [ ! -d "$SHELLSPEC_DIR" ]; then
    echo "ERROR: ShellSpec repo not found at $SHELLSPEC_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$SHELLSPEC_DIR"

# Check if shellspec executable exists
if [ ! -f "shellspec" ]; then
    echo "ERROR: shellspec executable not found"
    echo "The ShellSpec repo structure may have changed."
    exit 1
fi

echo "Running ShellSpec's own tests with silex as the shell..."
echo "(This may take 5-15 minutes)"
echo ""

# Run ShellSpec using silex as the interpreter
# This is a meta-test: ShellSpec tests itself using silex

# First, check if shellspec can even start with silex
echo "Testing if ShellSpec can start with silex..."
"$SILEX" ./shellspec --version 2>&1 | head -20
START_EXIT=$?

if [ $START_EXIT -ne 0 ]; then
    echo ""
    echo "ShellSpec failed to start. Testing basic shell features..."
    echo ""

    # Test which shell features are missing
    echo "Test 1: Command substitution"
    "$SILEX" -c 'x=$(echo test); echo $x' 2>&1

    echo ""
    echo "Test 2: Functions"
    "$SILEX" -c 'f() { echo test; }; f' 2>&1

    echo ""
    echo "Test 3: Local variables"
    "$SILEX" -c 'f() { local x=1; echo $x; }; f' 2>&1

    EXIT_CODE=$START_EXIT
else
    echo ""
    echo "ShellSpec started. Running full test suite..."
    "$SILEX" ./shellspec 2>&1
    EXIT_CODE=$?
fi

echo ""
echo "=== ShellSpec Test Results ==="
echo "Exit code: $EXIT_CODE"
echo ""

if [ $EXIT_CODE -eq 0 ]; then
    echo "Result: PASS"
    echo ""
    echo "Excellent! ShellSpec runs successfully on silex."
    echo "This validates silex's shell implementation is robust enough"
    echo "to run complex real-world shell programs."
    exit 0
else
    echo "Result: FAIL"
    echo ""
    echo "ShellSpec failed to run on silex. This indicates silex is"
    echo "missing critical shell features that complex programs depend on."
    echo ""
    echo "Review the errors above to identify what's missing."
    exit 1
fi
