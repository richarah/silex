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

# The vendored checkout was bootstrapped inside the Docker image at /silex:
# symlinks like GNUmakefile point at absolute paths that don't exist here,
# and GNU make prefers GNUmakefile over Makefile, so a dangling one breaks
# every make invocation. Re-point them at the sibling gnulib checkout (or
# drop them).
for lnk in GNUmakefile maint.mk; do
    if [ -L "$lnk" ] && [ ! -e "$lnk" ]; then
        tgt=$(readlink "$lnk" | sed "s|.*/repos/gnulib/|$REPOS_DIR/gnulib/|")
        if [ -e "$tgt" ]; then ln -sf "$tgt" "$lnk"; else rm -f "$lnk"; fi
    fi
done
find . -maxdepth 2 -type l ! -exec test -e {} \; -delete 2>/dev/null

# Configure if needed -- also RE-configure when the existing Makefile was
# generated somewhere else (the vendored checkout was configured inside the
# Docker image at /silex, leaving dangling absolute paths and symlinks).
NEED_CONF=0
if [ ! -f "Makefile" ]; then
    NEED_CONF=1
else
    conf_dir=$(sed -n 's/^abs_top_builddir = //p' Makefile | head -1)
    [ "$conf_dir" = "$(pwd)" ] || NEED_CONF=1
fi
if [ "$NEED_CONF" -eq 1 ]; then
    echo "Configuring GNU sed (stale or missing build config)..."
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

# Run tests. The tool overrides stop automake's maintainer-mode rules from
# trying to regenerate aclocal.m4/configure with whatever (mismatched)
# autotools this machine has -- we only want to BUILD and TEST. */
# help2man runs through perl, so a `true` override breaks it -- build the
# binary first, then satisfy the manpage rule with a stub before check.
make sed/sed AUTOCONF=true AUTOMAKE=true ACLOCAL=true AUTOHEADER=true AUTOM4TE=true MAKEINFO=true 2>&1 | tail -5 || true
mkdir -p doc && { [ -f doc/sed.1 ] || echo '.TH SED 1' > doc/sed.1; }
touch doc/sed.1
# The testsuite invokes the BUILT sed/sed binary, not PATH -- replace it with
# a wrapper that execs silex's sed so the suite actually measures silex.
if [ -f sed/sed ] && [ ! -f sed/sed.gnu ]; then
    mv sed/sed sed/sed.gnu
fi
printf '#!/bin/sh\nexec "%s" sed "$@"\n' "$SILEX" > sed/sed
chmod +x sed/sed
touch sed/sed
touch doc/sed.1   # keep the manpage newer than the wrapper binary
make check AUTOCONF=true AUTOMAKE=true ACLOCAL=true AUTOHEADER=true AUTOM4TE=true MAKEINFO=true 2>&1 | tail -30 || true

echo ""
echo "=== GNU sed Test Results ==="
echo ""

if [ -f "test-suite.log" ] || [ -f "tests/test-suite.log" ]; then
    [ -f "tests/test-suite.log" ] || ln -sf ../test-suite.log tests/test-suite.log 2>/dev/null || true
    TOTAL=$(grep -hE "^# TOTAL:" test-suite.log tests/test-suite.log 2>/dev/null | head -1 | awk '{print $3}' || echo "?")
    PASS=$(grep -hE "^# PASS:" test-suite.log tests/test-suite.log 2>/dev/null | head -1 | awk '{print $3}' || echo "?")
    FAIL=$(grep -hE "^# FAIL:" test-suite.log tests/test-suite.log 2>/dev/null | head -1 | awk '{print $3}' || echo "?")
    SKIP=$(grep -hE "^# SKIP:" test-suite.log tests/test-suite.log 2>/dev/null | head -1 | awk '{print $3}' || echo "?")

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
