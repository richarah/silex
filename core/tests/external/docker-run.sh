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

# Every suite invocation used to end in `|| echo "... timed out or failed"`,
# which turned each failure into a log line and left this script exiting 0. It
# is the only path CI exercises, so nothing downstream could ever go red.
#
# Now each suite's exit status is recorded, and the script fails if any suite
# failed -- including a suite that produced no output at all.
STATUS_DIR="$(mktemp -d)"
trap 'rm -rf "$STATUS_DIR"' EXIT INT TERM

# The suites we expect to have run. A suite with no status file at the end was
# killed before it could record one -- that counts as a failure, not a pass.
EXPECTED_SUITES="oils smoosh modernish mksh shellspec gnu-coreutils gnu-grep gnu-sed toybox configure"

# run <name> <timeout> <script> <result-file>
#
# The `if` is load-bearing: `set -e` is on, so a bare failing `timeout` would
# abort before the status was recorded -- and in a backgrounded call it would
# kill the subshell silently, leaving no status file at all. Inside an `if`
# condition, set -e is suspended.
run_suite() {
    _name="$1"; _to="$2"; _script="$3"; _out="$4"
    if timeout "$_to" "$_script" > "$_out" 2>&1; then
        _st=0
    else
        _st=$?
    fi
    echo "$_st" > "$STATUS_DIR/$_name"
}

# === PARALLEL BATCH 1: Shell suites ===
echo ""
echo "▶ Batch 1: Shell suites (5 parallel jobs)..."
echo "  - Oils/OSH, Smoosh, modernish, mksh, ShellSpec"

run_suite oils      600 tests/external/run-oils-spec.sh  "$RESULTS/oils-$TIMESTAMP.txt" &
run_suite smoosh    600 tests/external/run-smoosh.sh     "$RESULTS/smoosh-$TIMESTAMP.txt" &
run_suite modernish 600 tests/external/run-modernish.sh  "$RESULTS/modernish-$TIMESTAMP.txt" &
run_suite mksh      600 tests/external/run-mksh-tests.sh "$RESULTS/mksh-$TIMESTAMP.txt" &
run_suite shellspec 600 tests/external/run-shellspec.sh  "$RESULTS/shellspec-$TIMESTAMP.txt" &
wait
echo "✓ Shell suites complete"

# === PARALLEL BATCH 2: Coreutils suites ===
echo ""
echo "▶ Batch 2: Coreutils suites (4 parallel jobs)..."
echo "  - GNU coreutils, GNU grep, GNU sed, toybox"

run_suite gnu-coreutils 1800 tests/external/run-gnu-coreutils.sh "$RESULTS/gnu-coreutils-$TIMESTAMP.txt" &
run_suite gnu-grep       600 tests/external/run-gnu-grep.sh      "$RESULTS/gnu-grep-$TIMESTAMP.txt" &
run_suite gnu-sed        600 tests/external/run-gnu-sed.sh       "$RESULTS/gnu-sed-$TIMESTAMP.txt" &
run_suite toybox         600 tests/external/run-toybox.sh        "$RESULTS/toybox-$TIMESTAMP.txt" &
wait
echo "✓ Coreutils suites complete"

# === SEQUENTIAL: Configure scripts ===
echo ""
echo "▶ Batch 3: Configure scripts (sequential)..."
echo "  - curl, CPython, OpenSSL, SQLite, zlib"

run_suite configure 1200 tests/external/run-configure.sh "$RESULTS/configure-$TIMESTAMP.txt"
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

# === EXIT STATUS ===
# This script used to have no exit at all, so it always returned 0. CI runs
# `make external-test-docker` and nothing else, which is why a run where eight
# of ten suites executed zero tests still reported green.
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  SUITE EXIT STATUS"
echo "════════════════════════════════════════════════════════════════"

failed=0
for name in $EXPECTED_SUITES; do
    f="$STATUS_DIR/$name"
    if [ ! -e "$f" ]; then
        # No status recorded: the suite was killed before it could finish.
        printf '  %-16s DID NOT RUN\n' "$name"
        failed=$((failed + 1))
        continue
    fi
    st=$(cat "$f")
    if [ "$st" -eq 0 ]; then
        printf '  %-16s ok\n' "$name"
    elif [ "$st" -eq 124 ]; then
        printf '  %-16s TIMED OUT\n' "$name"
        failed=$((failed + 1))
    else
        printf '  %-16s FAILED (exit %s)\n' "$name" "$st"
        failed=$((failed + 1))
    fi
done

echo ""
if [ "$failed" -gt 0 ]; then
    echo "✗ $failed of 10 suite(s) failed. Results in $RESULTS/"
    exit 1
fi
echo "✓ All 10 suites passed."
