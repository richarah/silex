#!/bin/sh
# run-all.sh — Master external test suite runner for silex
# Runs all 10 external test suites and reports aggregated results.
#
# Usage: ./run-all.sh [SILEX_BINARY]
#   SILEX_BINARY  path to silex (default: ../../build/bin/silex)
#
# Exit codes:
#   0  All suites passed
#   1  One or more suites failed

set -u  # Error on undefined variables

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SILEX="${1:-$SCRIPT_DIR/../../build/bin/silex}"
REPOS_DIR="$SCRIPT_DIR/repos"
RESULTS_DIR="$SCRIPT_DIR/results"

PASS=0
FAIL=0
SKIP=0

# Results timestamp for tracking
TIMESTAMP=$(date +%Y%m%d-%H%M%S)

echo "=== silex External Test Suite Runner ==="
echo "Binary: $SILEX"
echo "Results: $RESULTS_DIR"
echo "Timestamp: $TIMESTAMP"
echo ""

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    echo "Build silex first: make release"
    exit 1
fi

# Verify repos are fetched
if [ ! -d "$REPOS_DIR/oil" ]; then
    echo "ERROR: External test repos not found"
    echo "Fetch repos first: $SCRIPT_DIR/fetch-all.sh"
    echo ""
    echo "Quick start:"
    echo "  cd $SCRIPT_DIR"
    echo "  ./fetch-all.sh"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

# Export SILEX for child scripts
export SILEX

run_suite() {
    local name="$1"
    local script="$2"
    local result_file="$3"

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "SUITE: $name"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    if [ ! -f "$script" ]; then
        echo "SKIP: Runner script not found: $script"
        SKIP=$((SKIP + 1))
        echo "status=skipped" > "$result_file"
        echo ""
        return
    fi

    # Run suite and capture output
    local start_time=$(date +%s)
    if sh "$script" 2>&1 | tee "$result_file"; then
        local exit_code=0
    else
        local exit_code=$?
    fi
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    # Append metadata to result file
    echo "" >> "$result_file"
    echo "=== Test Metadata ===" >> "$result_file"
    echo "Suite: $name" >> "$result_file"
    echo "Timestamp: $TIMESTAMP" >> "$result_file"
    echo "Duration: ${duration}s" >> "$result_file"
    echo "Exit code: $exit_code" >> "$result_file"

    if [ $exit_code -eq 0 ]; then
        echo "✓ PASS"
        PASS=$((PASS + 1))
    else
        echo "✗ FAIL (exit code: $exit_code)"
        FAIL=$((FAIL + 1))
    fi

    echo ""
}

# SUITE 1: Oils/OSH spec tests (~1500 POSIX shell tests)
# Critical for shell correctness
run_suite "Oils/OSH Spec Tests" \
    "$SCRIPT_DIR/run-oils-spec.sh" \
    "$RESULTS_DIR/oils-spec-$TIMESTAMP.txt"

# SUITE 2: Smoosh (157 tests from formal semantics)
# Gold standard for POSIX conformance
run_suite "Smoosh (Formal Semantics)" \
    "$SCRIPT_DIR/run-smoosh.sh" \
    "$RESULTS_DIR/smoosh-$TIMESTAMP.txt"

# SUITE 3: modernish (bug catalogue)
# CRITICAL: FTL (fatal) count must be 0
run_suite "modernish (Bug Catalogue)" \
    "$SCRIPT_DIR/run-modernish.sh" \
    "$RESULTS_DIR/modernish-$TIMESTAMP.txt"

# SUITE 4: mksh test suite
# Production shell regression tests
run_suite "mksh Test Suite" \
    "$SCRIPT_DIR/run-mksh-tests.sh" \
    "$RESULTS_DIR/mksh-$TIMESTAMP.txt"

# SUITE 5: GNU coreutils (645 tests)
# Canonical Unix utility tests (what uutils/coreutils uses)
run_suite "GNU coreutils" \
    "$SCRIPT_DIR/run-gnu-coreutils.sh" \
    "$RESULTS_DIR/gnu-coreutils-$TIMESTAMP.txt"

# SUITE 6: GNU grep (200+ tests)
# Authoritative grep behavior tests
run_suite "GNU grep" \
    "$SCRIPT_DIR/run-gnu-grep.sh" \
    "$RESULTS_DIR/gnu-grep-$TIMESTAMP.txt"

# SUITE 7: GNU sed (100+ tests)
# Reference sed implementation tests
run_suite "GNU sed" \
    "$SCRIPT_DIR/run-gnu-sed.sh" \
    "$RESULTS_DIR/gnu-sed-$TIMESTAMP.txt"

# SUITE 8: toybox
# Upstream tests (silex is a toybox fork)
run_suite "toybox" \
    "$SCRIPT_DIR/run-toybox.sh" \
    "$RESULTS_DIR/toybox-$TIMESTAMP.txt"

# SUITE 9: ShellSpec
# Meta-test: BDD framework in POSIX sh
run_suite "ShellSpec (Meta-test)" \
    "$SCRIPT_DIR/run-shellspec.sh" \
    "$RESULTS_DIR/shellspec-$TIMESTAMP.txt"

# SUITE 10: Autoconf configure scripts
# Real-world validation (CRITICAL: must pass 100%)
run_suite "Autoconf Configure Scripts" \
    "$SCRIPT_DIR/run-configure.sh" \
    "$RESULTS_DIR/configure-$TIMESTAMP.txt"

# Summary
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SUMMARY"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
printf 'Suites passed:  %d\n' "$PASS"
printf 'Suites failed:  %d\n' "$FAIL"
printf 'Suites skipped: %d\n' "$SKIP"
printf 'Total suites:   %d\n' $((PASS + FAIL + SKIP))
echo ""

# Check for critical failures
CRITICAL_FAILURES=0

# Check modernish FTL count (must be 0)
if [ -f "$RESULTS_DIR/modernish-$TIMESTAMP.txt" ]; then
    MODERNISH_FTL=$(grep -c "FTL" "$RESULTS_DIR/modernish-$TIMESTAMP.txt" || true)
    if [ "$MODERNISH_FTL" -gt 0 ]; then
        echo "CRITICAL: modernish has $MODERNISH_FTL fatal (FTL) bugs"
        CRITICAL_FAILURES=$((CRITICAL_FAILURES + 1))
    fi
fi

# Check configure scripts (must pass 100%)
if [ -f "$RESULTS_DIR/configure-$TIMESTAMP.txt" ]; then
    CONFIGURE_FAILS=$(grep -c "FAIL:" "$RESULTS_DIR/configure-$TIMESTAMP.txt" || true)
    if [ "$CONFIGURE_FAILS" -gt 0 ]; then
        echo "CRITICAL: $CONFIGURE_FAILS configure scripts failed"
        CRITICAL_FAILURES=$((CRITICAL_FAILURES + 1))
    fi
fi

if [ $CRITICAL_FAILURES -gt 0 ]; then
    echo ""
    echo "WARNING: $CRITICAL_FAILURES critical failure(s) detected"
    echo "These must be fixed before release:"
    echo "  - modernish FTL count must be 0"
    echo "  - All configure scripts must pass"
fi

echo ""
echo "Results saved to: $RESULTS_DIR/*-$TIMESTAMP.txt"
echo ""

# Exit code: 0 if all passed, 1 if any failed
[ "$FAIL" -eq 0 ]
