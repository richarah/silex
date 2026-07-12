#!/bin/sh
# run-gnu-sed.sh — Run GNU sed test suite against silex
# Suite 7: 100+ tests from the reference stream editor
#
# GNU sed is the reference implementation for stream editing.
# Tests cover substitutions, addresses, hold space, and complex patterns.
#
# Expected pass rate: 50-60% (sed is complex)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
SED_DIR="$REPOS_DIR/sed"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== GNU sed Test Suite ==="
echo "Suite: GNU sed (reference stream editor)"
echo "Tests: 100+ tests"
echo "Binary: $SILEX"
echo ""

# Verify sed repo exists
if [ ! -d "$SED_DIR" ]; then
    echo "ERROR: GNU sed repo not found at $SED_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$SED_DIR"

# Bootstrap if needed
if [ ! -f "configure" ] && [ -f "bootstrap" ]; then
    echo "Running bootstrap..."
    # gnulib needs to be a sibling directory
    if [ ! -d "../gnulib" ]; then
        echo "Cloning gnulib (required by bootstrap)..."
        cd ..
        git clone --depth 1 https://git.savannah.gnu.org/git/gnulib.git gnulib 2>&1 | tail -5
        cd sed
    fi
    GNULIB_SRCDIR=../gnulib ./bootstrap 2>&1 | tail -10
fi

# Check dependencies
for cmd in gcc make; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "WARNING: $cmd not found"
        echo "Skipping GNU sed tests."
        exit 0
    fi
done

# Configure if needed
if [ ! -f "Makefile" ]; then
    echo "Configuring GNU sed..."
    ./configure --quiet 2>&1 | tail -5
fi

# Verify Makefile was created
if [ ! -f "Makefile" ]; then
    echo "ERROR: Makefile not found after configure"
    echo "Bootstrap or configure failed."
    echo ""
    echo "GNU sed: pass=0 fail=0 total=0"
    exit 0
fi

echo ""
echo "Running GNU sed test suite..."
echo "(This may take 5-10 minutes)"
echo ""

# Set PATH to use silex's sed
TOOL_DIR="/tmp/silex-sed-$$"
mkdir -p "$TOOL_DIR"
trap 'rm -rf "$TOOL_DIR"' EXIT INT TERM
ln -sf "$SILEX" "$TOOL_DIR/sed"
export PATH="$TOOL_DIR:$PATH"

# Run tests
make check 2>&1 || true

echo ""
echo "=== GNU sed Test Results ==="
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

    echo "GNU sed: pass=$PASS fail=$FAIL total=$TOTAL"
else
    echo "ERROR: No test-suite.log found"
    echo "GNU sed: pass=0 fail=0 total=0"
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
