#!/bin/sh
# run-modernish.sh — Run modernish bug catalogue against silex
# Suite 3: Shell feature detection and bug catalogue
#
# modernish is a comprehensive shell feature detection library that maintains
# a catalogue of shell bugs classified as:
#   FTL (fatal): Critical bugs that break scripts
#   BUG:         Non-critical but incorrect behavior
#   QRK (quirk): Unusual but not necessarily wrong behavior
#
# **CRITICAL REQUIREMENT: FTL count MUST be 0**

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
MODERNISH_DIR="$REPOS_DIR/modernish"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== modernish Bug Catalogue Tests ==="
echo "Suite: modernish shell feature detection + bug catalogue"
echo "Binary: $SILEX"
echo ""
echo "CRITICAL: FTL (fatal bug) count must be 0"
echo ""

# Verify modernish repo exists
if [ ! -d "$MODERNISH_DIR" ]; then
    echo "ERROR: modernish repo not found at $MODERNISH_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$MODERNISH_DIR"

# Check if modernish test script exists
if [ ! -f "bin/modernish" ]; then
    echo "ERROR: bin/modernish not found"
    echo "The modernish repo structure may have changed."
    exit 1
fi

echo "Running modernish test suite with silex..."
echo "(This may take 3-10 minutes - comprehensive shell feature testing)"
echo ""

# Run modernish test suite
# --test: run test suite
# The test suite will detect bugs in the shell

# modernish needs to be run with the shell to test
RESULT_FILE="/tmp/silex-modernish-results.txt"
"$SILEX" bin/modernish --test 2>&1 | tee "$RESULT_FILE"
EXIT_CODE=$?

echo ""
echo "=== modernish Test Results ==="
echo ""

# Parse results for FTL (fatal), BUG, and QRK classifications
FTL_COUNT=$(grep -c "^FTL" "$RESULT_FILE" || true)
BUG_COUNT=$(grep -c "^BUG" "$RESULT_FILE" || true)
QRK_COUNT=$(grep -c "^QRK" "$RESULT_FILE" || true)

echo "Fatal bugs (FTL):     $FTL_COUNT"
echo "Non-critical bugs (BUG): $BUG_COUNT"
echo "Quirks (QRK):         $QRK_COUNT"
echo "Exit code:            $EXIT_CODE"
echo ""

# Check for critical failures
if [ "$FTL_COUNT" -gt 0 ]; then
    echo "CRITICAL FAILURE: $FTL_COUNT fatal bugs detected!"
    echo ""
    echo "Fatal bugs (FTL) are blockers that MUST be fixed before release."
    echo "These bugs break real-world shell scripts."
    echo ""
    echo "Failed tests:"
    grep "^FTL" "$RESULT_FILE" || true
    echo ""
    exit 1
fi

if [ "$BUG_COUNT" -gt 0 ]; then
    echo "WARNING: $BUG_COUNT non-critical bugs detected"
    echo ""
    echo "These should be triaged:"
    grep "^BUG" "$RESULT_FILE" | head -10 || true
    echo ""
fi

if [ "$QRK_COUNT" -gt 0 ]; then
    echo "INFO: $QRK_COUNT quirks detected"
    echo "Quirks are unusual behaviors that may or may not be intentional."
    echo ""
fi

# Success criteria: FTL = 0 (BUG and QRK are warnings, not failures)
if [ "$FTL_COUNT" -eq 0 ]; then
    echo "Result: PASS (no fatal bugs)"
    echo ""
    if [ "$BUG_COUNT" -eq 0 ] && [ "$QRK_COUNT" -eq 0 ]; then
        echo "Perfect score! No bugs or quirks detected."
    fi
    exit 0
else
    exit 1
fi
