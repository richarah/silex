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

# Fetch external repos (skip only if ALL are already present).
#
# This used to check for a single directory (repos/oil). A *partial* cache --
# oil present but, say, modernish or coreutils missing -- then reported "Using
# cached" and skipped the fetch, so every suite whose repo was absent failed
# with "repo not found". A GitHub Actions cache can restore partially, so this
# happened intermittently and made the whole gate's result depend on cache luck
# rather than on silex. Require every repo the run-*.sh scripts consume; fetch if
# any is missing. make external-fetch is idempotent (it updates existing repos
# and clones only what is absent), so re-running it is safe.
_missing=
for _repo in oil smoosh modernish mksh shellspec coreutils grep sed toybox projects; do
    if [ ! -d "tests/external/repos/$_repo" ]; then
        _missing="$_missing $_repo"
    fi
done
# gnulib is checked by CONTENT, not just presence. grep/sed/coreutils bootstrap
# against it, and a half-written tree -- from a partial cache restore, or from
# the clone race that used to happen when all three fetched it themselves --
# satisfies `[ -d ]` while still failing bootstrap with "does not contain
# gnulib-tool". The directory existing is not the property we need.
if [ ! -f "tests/external/repos/gnulib/gnulib-tool" ]; then
    _missing="$_missing gnulib"
fi
if [ -n "$_missing" ]; then
    echo ""
    echo "Fetching external test repositories (missing:$_missing)..."
    make external-fetch
else
    echo ""
    echo "Using cached external repositories (all present)"
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

ZERO_TEST_SUITES=""

parse_results() {
    local file="$1"
    local name="$2"

    if [ ! -f "$file" ]; then
        printf "  %-25s ERROR: Result file not found\n" "$name"
        return
    fi

    # Read the canonical line each runner emits:  <label>: pass=N fail=N total=N
    #
    # The previous approach grepped the whole file for /(pass|fail|total)[:=]/
    # and took `grep -oE '[0-9]+' | head -1` -- the FIRST number on the first
    # matching line, for all three counters. Against a line like
    # "TOTAL: 583" followed by "PASS: 201" it reported pass=201 fail=201
    # total=201 for mksh, whose real result was 201/382/583, and 16/16/16 for
    # toybox, whose real result was 16/1/17. It never read the same number
    # twice by accident; it read the same number three times by construction.
    local summary
    summary=$(grep -oE 'pass=[0-9]+ fail=[0-9]+ total=[0-9]+' "$file" | tail -1)
    if [ -n "$summary" ]; then
        local pass fail total
        pass=$(printf '%s' "$summary" | sed 's/.*pass=\([0-9]*\).*/\1/')
        fail=$(printf '%s' "$summary" | sed 's/.*fail=\([0-9]*\).*/\1/')
        total=$(printf '%s' "$summary" | sed 's/.*total=\([0-9]*\).*/\1/')
        if [ "$total" = "0" ]; then
            # A suite that ran nothing is not a suite that passed. Recorded so
            # the critical-requirements section can block on it.
            printf "  %-25s RAN NO TESTS (total=0)\n" "$name"
            ZERO_TEST_SUITES="$ZERO_TEST_SUITES $name"
        else
            printf "  %-25s pass: %-6s fail: %-6s total: %s\n" "$name" "$pass" "$fail" "$total"
        fi
        return
    fi

    # Runners that emit a verdict but no counts. Report the verdict as-is
    # rather than inventing numbers for it.
    local verdict
    verdict=$(grep -oE '^Result: .*' "$file" | tail -1)
    if [ -n "$verdict" ]; then
        printf "  %-25s %s\n" "$name" "$verdict"
        return
    fi

    printf "  %-25s NO MACHINE-READABLE SUMMARY\n" "$name"
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

# Check 3: no suite may report zero tests.
#
# The per-suite exit-status block below catches a suite that CRASHED, but not
# one that exited 0 having run nothing. gnu-coreutils, gnu-grep and gnu-sed all
# did exactly that -- "./configure: not found", zero tests, exit 0 -- and sailed
# through the gate, which is the same class of hole the exit-status block was
# added to close.
if [ -n "$ZERO_TEST_SUITES" ]; then
    echo "✗ suite(s) ran zero tests:$ZERO_TEST_SUITES (BLOCKER)"
    ZERO_TEST_FAIL=1
else
    echo "✓ every suite with counts ran at least one test (PASS)"
    ZERO_TEST_FAIL=0
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
if [ "${ZERO_TEST_FAIL:-0}" -ne 0 ]; then
    echo "✗ suite(s) ran zero tests:$ZERO_TEST_SUITES"
    exit 1
fi
echo "✓ All 10 suites passed."
