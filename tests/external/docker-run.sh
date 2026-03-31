#!/bin/bash
# docker-run.sh — Run all external test suites inside Docker container
# This script runs IN THE CONTAINER with proper Linux filesystem

set -e

echo "════════════════════════════════════════════════════════════════"
echo "  silex External Test Suite Runner (Docker)"
echo "════════════════════════════════════════════════════════════════"
echo "Date: $(date)"
echo "Host: $(hostname)"
echo ""

# Build silex
echo "Building silex..."
make clean > /dev/null 2>&1 || true
make release-glibc

# Verify binary works
echo "Testing binary..."
./build/bin/silex --version

# Fetch external repos (skip if cached via volume mount)
if [ ! -d tests/external/repos/oil ]; then
    echo ""
    echo "Fetching external test repositories (first run only)..."
    make external-fetch
else
    echo ""
    echo "Using cached external repositories"
fi

# Results directory
RESULTS="tests/external/results"
mkdir -p "$RESULTS"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  Running Test Suites (Parallel Execution)"
echo "════════════════════════════════════════════════════════════════"

# === PARALLEL BATCH 1: Shell suites ===
echo ""
echo "▶ Batch 1: Shell suites (5 parallel jobs)..."
echo "  - Oils/OSH, Smoosh, modernish, mksh, ShellSpec"

(timeout 600 tests/external/run-oils-spec.sh > "$RESULTS/oils-$TIMESTAMP.txt" 2>&1 || echo "Oils timed out or failed") &
PID_OILS=$!

(timeout 600 tests/external/run-smoosh.sh > "$RESULTS/smoosh-$TIMESTAMP.txt" 2>&1 || echo "Smoosh timed out or failed") &
PID_SMOOSH=$!

(timeout 600 tests/external/run-modernish.sh > "$RESULTS/modernish-$TIMESTAMP.txt" 2>&1 || echo "modernish timed out or failed") &
PID_MODERN=$!

(timeout 600 tests/external/run-mksh-tests.sh > "$RESULTS/mksh-$TIMESTAMP.txt" 2>&1 || echo "mksh timed out or failed") &
PID_MKSH=$!

(timeout 600 tests/external/run-shellspec.sh > "$RESULTS/shellspec-$TIMESTAMP.txt" 2>&1 || echo "ShellSpec timed out or failed") &
PID_SS=$!

# Wait for all shell suites
for pid in $PID_OILS $PID_SMOOSH $PID_MODERN $PID_MKSH $PID_SS; do
    wait $pid 2>/dev/null || true
done
echo "✓ Shell suites complete"

# === PARALLEL BATCH 2: Coreutils suites ===
echo ""
echo "▶ Batch 2: Coreutils suites (4 parallel jobs)..."
echo "  - GNU coreutils, GNU grep, GNU sed, toybox"

(timeout 1800 tests/external/run-gnu-coreutils.sh > "$RESULTS/gnu-coreutils-$TIMESTAMP.txt" 2>&1 || echo "GNU coreutils timed out or failed") &
PID_GNU=$!

(timeout 600 tests/external/run-gnu-grep.sh > "$RESULTS/gnu-grep-$TIMESTAMP.txt" 2>&1 || echo "GNU grep timed out or failed") &
PID_GREP=$!

(timeout 600 tests/external/run-gnu-sed.sh > "$RESULTS/gnu-sed-$TIMESTAMP.txt" 2>&1 || echo "GNU sed timed out or failed") &
PID_SED=$!

(timeout 600 tests/external/run-toybox.sh > "$RESULTS/toybox-$TIMESTAMP.txt" 2>&1 || echo "toybox timed out or failed") &
PID_TOY=$!

for pid in $PID_GNU $PID_GREP $PID_SED $PID_TOY; do
    wait $pid 2>/dev/null || true
done
echo "✓ Coreutils suites complete"

# === SEQUENTIAL: Configure scripts ===
echo ""
echo "▶ Batch 3: Configure scripts (sequential)..."
echo "  - curl, CPython, OpenSSL, SQLite, zlib"

timeout 1200 tests/external/run-configure.sh > "$RESULTS/configure-$TIMESTAMP.txt" 2>&1 || echo "Configure timed out or failed"
echo "✓ Configure tests complete"

# === GENERATE SCORECARD ===
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  SCORECARD"
echo "════════════════════════════════════════════════════════════════"
echo ""

parse_results() {
    local file="$1"
    local name="$2"

    if [ ! -f "$file" ]; then
        printf "  %-25s ERROR: Result file not found\n" "$name"
        return
    fi

    # Try multiple patterns to extract pass/fail counts
    local pass=$(grep -iE '(passed|pass|ok)[:\s=]+[0-9]+' "$file" | grep -oE '[0-9]+' | head -1 || echo "?")
    local fail=$(grep -iE '(failed|fail|not ok)[:\s=]+[0-9]+' "$file" | grep -oE '[0-9]+' | head -1 || echo "?")
    local total=$(grep -iE 'total[:\s=]+[0-9]+' "$file" | grep -oE '[0-9]+' | head -1 || echo "?")

    # If we couldn't parse, check for specific patterns
    if [ "$pass" = "?" ] && [ "$fail" = "?" ]; then
        # Check for SKIP/PASS markers
        if grep -q "SKIP:" "$file"; then
            printf "  %-25s SKIPPED\n" "$name"
            return
        elif grep -q "ERROR:" "$file"; then
            printf "  %-25s ERROR\n" "$name"
            return
        fi
    fi

    printf "  %-25s pass: %-6s fail: %-6s total: %s\n" "$name" "$pass" "$fail" "$total"
}

# Parse each suite
parse_results "$RESULTS/oils-$TIMESTAMP.txt" "Oils/OSH"
parse_results "$RESULTS/smoosh-$TIMESTAMP.txt" "Smoosh"
parse_results "$RESULTS/modernish-$TIMESTAMP.txt" "modernish"
parse_results "$RESULTS/mksh-$TIMESTAMP.txt" "mksh"
parse_results "$RESULTS/gnu-coreutils-$TIMESTAMP.txt" "GNU coreutils"
parse_results "$RESULTS/gnu-grep-$TIMESTAMP.txt" "GNU grep"
parse_results "$RESULTS/gnu-sed-$TIMESTAMP.txt" "GNU sed"
parse_results "$RESULTS/toybox-$TIMESTAMP.txt" "toybox"
parse_results "$RESULTS/shellspec-$TIMESTAMP.txt" "ShellSpec"
parse_results "$RESULTS/configure-$TIMESTAMP.txt" "Autoconf Configure"

# === CRITICAL CHECKS ===
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  CRITICAL REQUIREMENTS"
echo "════════════════════════════════════════════════════════════════"
echo ""

# Check 1: modernish FTL count must be 0
if [ -f "$RESULTS/modernish-$TIMESTAMP.txt" ]; then
    FTL=$(grep -i 'Fatal bugs (FTL):' "$RESULTS/modernish-$TIMESTAMP.txt" | grep -oE '[0-9]+' | head -1 || echo "unknown")
    if [ "$FTL" = "0" ]; then
        echo "✓ modernish FTL count = 0 (PASS)"
    else
        echo "✗ modernish FTL count = $FTL (BLOCKER - must be 0)"
    fi
else
    echo "? modernish results not found"
fi

# Check 2: Configure scripts must all pass
if [ -f "$RESULTS/configure-$TIMESTAMP.txt" ]; then
    CONF_FAIL=$(grep -i 'Failed:' "$RESULTS/configure-$TIMESTAMP.txt" | grep -oE '[0-9]+' | head -1 || echo "unknown")
    if [ "$CONF_FAIL" = "0" ]; then
        echo "✓ All configure scripts passed (PASS)"
    else
        echo "✗ Configure failures = $CONF_FAIL (BLOCKER - must be 0)"
    fi
else
    echo "? Configure results not found"
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  Results saved to: $RESULTS/*-$TIMESTAMP.txt"
echo "════════════════════════════════════════════════════════════════"
echo ""
