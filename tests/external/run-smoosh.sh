#!/bin/sh
# run-smoosh.sh — Run the Smoosh conformance suite against silex.
#
# Smoosh is a POSIX shell formalized in Coq. Its tests are derived from the
# formal model, so they are the closest thing to a ground truth for POSIX
# shell semantics.
#
# This delegates to Smoosh's OWN runner (tests/shell_tests.sh) rather than
# reimplementing it. That matters:
#
#   * Upstream runs every test in a fresh mktemp dir. A reimplementation that
#     runs them in-place scatters each test's scratch files (a1, cmd.sh, dir/,
#     link_*) through the source tree -- which is how they ended up committed.
#   * Upstream defaults the expected exit code to 0 when there is no .ec file.
#     Skipping that check instead of defaulting it silently passes every test
#     that exits non-zero when it should have exited 0.
#   * Upstream exits non-zero when any test fails.
#
# Do not reimplement it again.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SMOOSH_DIR="$SCRIPT_DIR/repos/smoosh"

# Canonicalise before anything cds: a relative path is meaningless afterwards.
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"
case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" 2>/dev/null && pwd)/$(basename "$SILEX")" ;;
esac

if [ ! -d "$SMOOSH_DIR" ]; then
    echo "ERROR: Smoosh repo not found at $SMOOSH_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

echo "=== Smoosh Formal Semantics Tests ==="
echo "Binary: $SILEX"
echo ""

# The suite's C helpers (fds, argv, getenv, readdir) are used by some tests.
if ! make -C "$SMOOSH_DIR/tests" utils >/dev/null 2>&1; then
    echo "ERROR: could not build Smoosh test utilities (make -C tests utils)"
    exit 1
fi

# Keep the run's logs out of the source tree.
LOG_DIR="$(mktemp -d)"
trap 'rm -rf "$LOG_DIR"' EXIT INT TERM

cd "$SMOOSH_DIR/tests"

TEST_SHELL="$SILEX" \
TEST_UTIL="$SMOOSH_DIR/tests/util" \
TEST_LOGDIR="$LOG_DIR" \
TEST_ENV="" \
    sh ./shell_tests.sh
status=$?

echo ""
if [ "$status" -eq 0 ]; then
    echo "Result: PASS (all tests passed)"
else
    echo "Result: FAIL"
fi

exit "$status"
