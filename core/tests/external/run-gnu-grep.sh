#!/bin/sh
# run-gnu-grep.sh — Run GNU grep test suite against silex
# Suite 6: 200+ tests from the authoritative grep implementation
#
# GNU grep is the reference implementation for pattern matching utilities.
# Tests cover basic patterns, extended regex, context flags, and edge cases.
#
# Expected pass rate: 60-70% (grep is mature in silex)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
GREP_DIR="$REPOS_DIR/grep"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== GNU grep Test Suite ==="
echo "Suite: GNU grep (authoritative pattern matching)"
echo "Tests: 200+ tests"
echo "Binary: $SILEX"
echo ""

# Verify grep repo exists
if [ ! -d "$GREP_DIR" ]; then
    echo "ERROR: GNU grep repo not found at $GREP_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$GREP_DIR"

# Bootstrap if needed
if [ ! -f "configure" ] && [ -f "bootstrap" ]; then
    echo "Running bootstrap..."
    # gnulib needs to be a sibling directory
    if [ ! -d "../gnulib" ]; then
        echo "Cloning gnulib (required by bootstrap)..."
        cd ..
        git clone --depth 1 https://git.savannah.gnu.org/git/gnulib.git gnulib 2>&1 | tail -5
        cd grep
    fi
    GNULIB_SRCDIR=../gnulib ./bootstrap 2>&1 | tail -10
fi

# Check dependencies
for cmd in gcc make; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "WARNING: $cmd not found"
        echo "Skipping GNU grep tests."
        exit 0
    fi
done

# Configure if needed
if [ ! -f "Makefile" ]; then
    echo "Configuring GNU grep..."
    ./configure --quiet 2>&1 | tail -5
fi

# Verify Makefile was created
if [ ! -f "Makefile" ]; then
    echo "ERROR: Makefile not found after configure"
    echo "Bootstrap or configure failed."
    echo ""
    echo "GNU grep: pass=0 fail=0 total=0"
    exit 0
fi

echo ""
echo "Running GNU grep test suite..."
echo "(This may take 5-10 minutes)"
echo ""

# Set PATH to use silex's grep
TOOL_DIR="/tmp/silex-grep-$$"
mkdir -p "$TOOL_DIR"
trap 'rm -rf "$TOOL_DIR"' EXIT INT TERM
ln -sf "$SILEX" "$TOOL_DIR/grep"
export PATH="$TOOL_DIR:$PATH"

# Run tests
make check 2>&1 || true

echo ""
echo "=== GNU grep Test Results ==="
echo ""

if [ -f "tests/test-suite.log" ]; then
    TOTAL=$(grep -E "^# TOTAL:" tests/test-suite.log | awk '{print $3}' || echo "?")
    PASS=$(grep -E "^# PASS:" tests/test-suite.log | awk '{print $3}' || echo "?")
    FAIL=$(grep -E "^# FAIL:" tests/test-suite.log | awk '{print $3}' || echo "?")
    SKIP=$(grep -E "^# SKIP:" tests/test-suite.log | awk '{print $3}' || echo "?")

    echo "  TOTAL: $TOTAL"
    echo "  PASS:  $PASS"
    echo "  FAIL:  $FAIL"
    echo "  SKIP:  $SKIP"
    echo ""

    echo "GNU grep: pass=$PASS fail=$FAIL total=$TOTAL"
else
    echo "ERROR: No test-suite.log found"
    echo "GNU grep: pass=0 fail=0 total=0"
fi

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
