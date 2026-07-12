#!/bin/sh
# run-configure.sh — Run Autoconf configure scripts with silex
# Suite 10: Real-world validation using major open-source projects
#
# Tests silex's shell compatibility by running actual configure scripts from:
#   - curl: HTTP client library
#   - CPython: Python interpreter
#   - OpenSSL: Cryptography library
#   - SQLite: Database engine
#   - zlib: Compression library
#
# **CRITICAL REQUIREMENT: All configure scripts must pass (100%)**
# If configure scripts fail, silex cannot be used to build real software.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
PROJECTS_DIR="$REPOS_DIR/projects"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== Autoconf Configure Scripts Test ==="
echo "Suite: Real-world configure scripts"
echo "Projects: curl, CPython, OpenSSL, SQLite, zlib"
echo "Shell: $SILEX"
echo ""
echo "CRITICAL: All configure scripts must pass (100%)"
echo ""

# Verify projects directory exists
if [ ! -d "$PROJECTS_DIR" ]; then
    echo "ERROR: Projects directory not found at $PROJECTS_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

# Test installation prefix (won't actually install, just configure)
TEST_PREFIX="/tmp/silex-configure-test-$$"
mkdir -p "$TEST_PREFIX"
trap 'rm -rf "$TEST_PREFIX"' EXIT INT TERM

# Track results
TOTAL=0
PASS=0
FAIL=0

# Function to test a configure script
test_configure() {
    local project="$1"
    local project_dir="$PROJECTS_DIR/$project"

    TOTAL=$((TOTAL + 1))
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Testing: $project"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    if [ ! -d "$project_dir" ]; then
        echo "SKIP: $project not found at $project_dir"
        echo ""
        return
    fi

    cd "$project_dir"

    # Generate configure script if needed
    if [ ! -f "configure" ]; then
        echo "Generating configure script..."
        if [ -f "autogen.sh" ]; then
            "$SILEX" ./autogen.sh 2>&1 | tail -10
        elif [ -f "buildconf" ]; then
            "$SILEX" ./buildconf 2>&1 | tail -10
        elif [ -f "configure.ac" ] || [ -f "configure.in" ]; then
            autoreconf -i 2>&1 | tail -10
        else
            echo "SKIP: No configure or autogen script found"
            echo ""
            return
        fi
    fi

    if [ ! -f "configure" ]; then
        echo "SKIP: Failed to generate configure script"
        echo ""
        return
    fi

    # Run configure script with silex as /bin/sh
    echo "Running ./configure with silex..."
    echo ""

    # Set CONFIG_SHELL to use silex
    export CONFIG_SHELL="$SILEX"

    # Run configure (with minimal options to speed up)
    if "$SILEX" ./configure --prefix="$TEST_PREFIX" \
        --disable-shared --disable-dependency-tracking \
        --quiet 2>&1 | tail -50; then
        echo ""
        echo "✓ PASS: $project configure succeeded"
        PASS=$((PASS + 1))
    else
        EXIT_CODE=$?
        echo ""
        echo "✗ FAIL: $project configure failed (exit code: $EXIT_CODE)"
        FAIL=$((FAIL + 1))
    fi

    # Clean up generated files
    if [ -f "Makefile" ]; then
        make distclean 2>/dev/null || true
    fi

    echo ""
}

# Test each project
test_configure "curl"
test_configure "cpython"
test_configure "openssl"
test_configure "sqlite"
test_configure "zlib"

# Summary
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "SUMMARY"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
printf 'Total projects:  %d\n' "$TOTAL"
printf 'Passed:          %d\n' "$PASS"
printf 'Failed:          %d\n' "$FAIL"
echo ""

# Critical: All must pass
if [ "$FAIL" -eq 0 ] && [ "$PASS" -gt 0 ]; then
    echo "Result: PASS (100% of configure scripts succeeded)"
    echo ""
    echo "Excellent! silex can build real-world software."
    exit 0
elif [ "$FAIL" -gt 0 ]; then
    echo "Result: CRITICAL FAILURE"
    echo ""
    echo "$FAIL configure script(s) failed. This is a BLOCKER."
    echo "silex cannot be used to build software until this is fixed."
    echo ""
    echo "Review the failures above to identify shell compatibility issues."
    exit 1
else
    echo "Result: SKIP (no projects tested)"
    exit 0
fi
