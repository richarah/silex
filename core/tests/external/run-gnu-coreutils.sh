#!/bin/sh
# run-gnu-coreutils.sh — Run GNU coreutils test suite against silex
# Suite 5: 645 tests from the canonical Unix utilities test suite
#
# GNU coreutils is THE reference implementation for Unix utilities.
# This is the same test suite that uutils/coreutils uses for validation.
#
# Tests: cat, cp, mv, rm, mkdir, ls, sort, wc, head, tail, cut, tr, etc.
# Expected pass rate: 30-50% initially (ambitious target: 70%+)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
COREUTILS_DIR="$REPOS_DIR/coreutils"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== GNU coreutils Test Suite ==="
echo "Suite: GNU coreutils (canonical Unix utilities)"
echo "Tests: 645 tests for cat, cp, mv, ls, sort, wc, etc."
echo "Binary: $SILEX"
echo ""

# Verify coreutils repo exists
if [ ! -d "$COREUTILS_DIR" ]; then
    echo "ERROR: GNU coreutils repo not found at $COREUTILS_DIR"
    echo "Run: tests/external/fetch-all.sh"
    exit 1
fi

# Verify silex binary exists
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX"
    exit 1
fi

cd "$COREUTILS_DIR"

# Check if configure exists
if [ ! -f "configure" ]; then
    echo "Running bootstrap to generate configure script..."
    if [ -f "bootstrap" ]; then
        # gnulib needs to be a sibling directory
        if [ ! -d "../gnulib" ]; then
            echo "Cloning gnulib (required by bootstrap)..."
            cd ..
            git clone --depth 1 https://git.savannah.gnu.org/git/gnulib.git gnulib 2>&1 | tail -5
            cd coreutils
        fi
        GNULIB_SRCDIR=../gnulib ./bootstrap --skip-po 2>&1 | tail -20
    else
        echo "ERROR: No configure or bootstrap script found"
        echo "Cannot build GNU coreutils tests."
        exit 1
    fi
fi

# Check for required build dependencies
for cmd in gcc make autoconf automake; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "WARNING: $cmd not found (required to build coreutils)"
        echo "Install build dependencies:"
        echo "  Ubuntu/Debian: apt-get install build-essential autoconf automake gettext texinfo"
        echo "  Alpine: apk add gcc make autoconf automake gettext-dev"
        echo ""
        echo "Skipping GNU coreutils tests."
        exit 0
    fi
done

# The vendored checkout may have been configured at a different path (the
# Docker image, or the pre-monorepo layout); a stale build config leaves
# dangling GNUmakefile/maint.mk symlinks (which GNU make prefers over
# Makefile) and wrong absolute paths. Re-point gnulib symlinks and
# reconfigure when that is detected.
for lnk in GNUmakefile maint.mk; do
    if [ -L "$lnk" ] && [ ! -e "$lnk" ]; then
        tgt=$(readlink "$lnk" | sed "s|.*/repos/gnulib/|$REPOS_DIR/gnulib/|")
        if [ -e "$tgt" ]; then ln -sf "$tgt" "$lnk"; else rm -f "$lnk"; fi
    fi
done
NEED_CONF=0
if [ ! -f "Makefile" ]; then
    NEED_CONF=1
else
    conf_dir=$(sed -n 's/^abs_top_builddir = //p' Makefile | head -1)
    [ "$conf_dir" = "$(pwd)" ] || NEED_CONF=1
fi
if [ "$NEED_CONF" -eq 1 ]; then
    if [ -x ../gnulib/gnulib-tool ]; then
        echo "Refreshing gnulib imports (dangling symlinks from a foreign build)..."
        ../gnulib/gnulib-tool --update --symlink >/dev/null 2>&1 || true
    fi
    echo "Configuring GNU coreutils (stale or missing build config)..."
    ./configure --quiet 2>&1 | tail -10
fi

echo ""
echo "Running GNU coreutils test suite..."
echo "(This may take 15-30 minutes - 645 tests)"
echo ""

# Key technique: Manipulate PATH so tests use silex tools instead of system tools
# Create a temporary directory with symlinks to silex for each tool
TOOL_DIR="/tmp/silex-tools-$$"
mkdir -p "$TOOL_DIR"
trap 'rm -rf "$TOOL_DIR"' EXIT INT TERM

# Create symlinks for each tool silex implements
for tool in cat cp mv rm mkdir ls sort wc grep sed head tail cut tr \
            basename dirname stat date touch chmod ln readlink realpath \
            env printf sha256sum tee mktemp install find xargs; do
    ln -sf "$SILEX" "$TOOL_DIR/$tool"
done

# The silex tools go on PATH only for the make check invocation below --
# exporting them for the whole runner made the runner's own pipelines (and
# automake's) use silex applets, and any missing flag broke the harness
# rather than failing a test.
SILEX_PATH="$TOOL_DIR:$PATH"

# Verify Makefile was created
if [ ! -f "Makefile" ]; then
    echo "ERROR: Makefile not found after configure"
    echo "Bootstrap or configure failed."
    echo ""
    echo "GNU coreutils: pass=0 fail=0 total=0"
    exit 0
fi

# Run a subset of tests (running all 645 takes too long), focused on tools
# silex implements. Globs are expanded HERE against whatever layout this
# coreutils version uses (tests moved from tests/misc/cat-*.sh to
# tests/cat/*.sh upstream) -- passing a nonexistent path in TESTS= makes
# the whole check die with 'failed to create ....trs'.
TEST_CATEGORIES=""
for g in tests/cat/*.sh tests/head/*.sh tests/tail/*.sh tests/wc/*.sh \
         tests/sort/*.sh tests/basename/*.sh tests/dirname/*.sh \
         tests/misc/cat-*.sh tests/misc/head*.sh tests/misc/tail*.sh \
         tests/misc/wc*.sh tests/misc/sort*.sh tests/misc/basename*.sh \
         tests/cp/*.sh tests/mv/*.sh tests/rm/*.sh tests/mkdir/*.sh \
         tests/ln/*.sh tests/chmod/*.sh tests/touch/*.sh; do
    [ -f "$g" ] && TEST_CATEGORIES="$TEST_CATEGORIES $g"
done

# Run tests. Tool overrides stop automake's maintainer-mode rules from
# regenerating configure/aclocal with this machine's (mismatched) autotools.
PATH="$SILEX_PATH" make check TESTS="$TEST_CATEGORIES" \
     AUTOCONF=true AUTOMAKE=true ACLOCAL=true AUTOHEADER=true \
     AUTOM4TE=true MAKEINFO=true 2>&1 | tail -30 || true

echo ""
echo "=== GNU coreutils Test Results ==="
echo ""

# Parse test-suite.log for results
if [ -f "tests/test-suite.log" ]; then
    echo "Summary from test-suite.log:"
    TOTAL=$(grep -E "^# TOTAL:" tests/test-suite.log | awk '{print $3}' || echo "?")
    PASS=$(grep -E "^# PASS:" tests/test-suite.log | awk '{print $3}' || echo "?")
    FAIL=$(grep -E "^# FAIL:" tests/test-suite.log | awk '{print $3}' || echo "?")
    SKIP=$(grep -E "^# SKIP:" tests/test-suite.log | awk '{print $3}' || echo "?")

    echo "  TOTAL: $TOTAL"
    echo "  PASS:  $PASS"
    echo "  FAIL:  $FAIL"
    echo "  SKIP:  $SKIP"
    echo ""

    # Print pass/fail for scorecard parsing
    echo "GNU coreutils: pass=$PASS fail=$FAIL total=$TOTAL"
else
    echo "ERROR: No test-suite.log found"
    echo "GNU coreutils: pass=0 fail=0 total=0"
fi

echo ""
echo "Result: Tests completed"
echo ""

# Don't fail on test failures (expected at this stage)
# A suite that executed zero tests has not passed -- it has not run. Eight of
# the ten suites were doing exactly that, and the hardcoded `exit 0` that used
# to sit here reported every one of them as green.
if [ "${TOTAL:-0}" -eq 0 ]; then
    echo "ERROR: no tests were executed. The suite did not run."
    exit 1
fi
[ "${FAIL:-1}" -eq 0 ]
