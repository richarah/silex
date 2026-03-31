#!/bin/sh
# triage.sh — Failure classification tool for external test suites
#
# Usage: ./triage.sh <result-file>
#
# Classifies test failures into 5 categories:
#   1. bug         - Real silex bug (needs fix)
#   2. ambiguity   - POSIX ambiguity (document & decide)
#   3. intentional - Known deviation (document rationale)
#   4. missing     - Unimplemented feature (tracked in backlog)
#   5. environment - Test infrastructure issue (skip/adapt)
#
# Maintains a triage database to track classifications over time.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"
TRIAGE_DB="$RESULTS_DIR/triage.db"

# Parse arguments
RESULT_FILE="${1:-}"

if [ -z "$RESULT_FILE" ]; then
    echo "Usage: $0 <result-file>"
    echo ""
    echo "Example:"
    echo "  $0 results/oils-spec-20260331-120000.txt"
    echo ""
    echo "Available result files:"
    ls -1t "$RESULTS_DIR"/*.txt 2>/dev/null | head -10 || echo "  (none yet - run tests first)"
    exit 1
fi

if [ ! -f "$RESULT_FILE" ]; then
    echo "ERROR: Result file not found: $RESULT_FILE"
    exit 1
fi

mkdir -p "$RESULTS_DIR"
touch "$TRIAGE_DB"

echo "=== External Test Failure Triage Tool ==="
echo "Result file: $RESULT_FILE"
echo "Triage DB: $TRIAGE_DB"
echo ""

# Extract failures from result file
# This is a heuristic - different test suites have different output formats
FAILURES=$(grep -E "FAIL|FAILED|ERROR|✗" "$RESULT_FILE" | head -50)

if [ -z "$FAILURES" ]; then
    echo "No failures detected in result file."
    echo "This could mean:"
    echo "  1. All tests passed (excellent!)"
    echo "  2. The test suite output format is not recognized"
    echo ""
    exit 0
fi

echo "Found failures in result file:"
echo ""
echo "$FAILURES" | head -20
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Interactive triage
echo "Triage Classification:"
echo "  1 = bug         (real silex bug - needs fix)"
echo "  2 = ambiguity   (POSIX ambiguity - document & decide)"
echo "  3 = intentional (known deviation - document rationale)"
echo "  4 = missing     (unimplemented feature - backlog)"
echo "  5 = environment (test infrastructure issue)"
echo "  s = skip this failure"
echo "  q = quit triage"
echo ""

# Count for statistics
TRIAGED=0

echo "$FAILURES" | while IFS= read -r failure_line; do
    # Extract test name (heuristic - varies by suite)
    TEST_NAME=$(echo "$failure_line" | sed 's/^[^:]*://' | cut -d' ' -f1-5)

    # Check if already triaged
    if grep -qF "$TEST_NAME" "$TRIAGE_DB" 2>/dev/null; then
        continue
    fi

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Failure: $TEST_NAME"
    echo "$failure_line"
    echo ""
    printf "Classify (1-5/s/q): "
    read -r classification

    case "$classification" in
        1)
            echo "$TEST_NAME|bug|$(date +%Y-%m-%d)" >> "$TRIAGE_DB"
            TRIAGED=$((TRIAGED + 1))
            echo "Marked as BUG"
            ;;
        2)
            echo "$TEST_NAME|ambiguity|$(date +%Y-%m-%d)" >> "$TRIAGE_DB"
            TRIAGED=$((TRIAGED + 1))
            echo "Marked as AMBIGUITY"
            ;;
        3)
            echo "$TEST_NAME|intentional|$(date +%Y-%m-%d)" >> "$TRIAGE_DB"
            TRIAGED=$((TRIAGED + 1))
            echo "Marked as INTENTIONAL"
            ;;
        4)
            echo "$TEST_NAME|missing|$(date +%Y-%m-%d)" >> "$TRIAGE_DB"
            TRIAGED=$((TRIAGED + 1))
            echo "Marked as MISSING"
            ;;
        5)
            echo "$TEST_NAME|environment|$(date +%Y-%m-%d)" >> "$TRIAGE_DB"
            TRIAGED=$((TRIAGED + 1))
            echo "Marked as ENVIRONMENT"
            ;;
        s)
            echo "Skipped"
            ;;
        q)
            echo "Exiting triage."
            break
            ;;
        *)
            echo "Invalid input. Skipping."
            ;;
    esac
    echo ""
done

echo ""
echo "=== Triage Summary ==="
echo ""

if [ -f "$TRIAGE_DB" ] && [ -s "$TRIAGE_DB" ]; then
    BUG_COUNT=$(grep -c "|bug|" "$TRIAGE_DB" || true)
    AMBIGUITY_COUNT=$(grep -c "|ambiguity|" "$TRIAGE_DB" || true)
    INTENTIONAL_COUNT=$(grep -c "|intentional|" "$TRIAGE_DB" || true)
    MISSING_COUNT=$(grep -c "|missing|" "$TRIAGE_DB" || true)
    ENVIRONMENT_COUNT=$(grep -c "|environment|" "$TRIAGE_DB" || true)

    echo "Bugs (need fix):        $BUG_COUNT"
    echo "Ambiguities (review):   $AMBIGUITY_COUNT"
    echo "Intentional (document): $INTENTIONAL_COUNT"
    echo "Missing features:       $MISSING_COUNT"
    echo "Environment issues:     $ENVIRONMENT_COUNT"
    echo ""
    echo "Total triaged: $((BUG_COUNT + AMBIGUITY_COUNT + INTENTIONAL_COUNT + MISSING_COUNT + ENVIRONMENT_COUNT))"
else
    echo "No failures triaged yet."
fi

echo ""
echo "Triage database: $TRIAGE_DB"
echo ""
