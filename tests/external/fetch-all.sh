#!/bin/sh
# fetch-all.sh — Fetch all external test suites for silex
# This is a one-time setup (~5-10 minutes) that clones 10 external test repositories.
# Uses shallow clones (--depth 1) to save disk space (~500MB total).
#
# Usage: ./fetch-all.sh
#
# External test suites provide battle-tested conformance validation from mature projects.
# We run THEIR tests against OUR code (NIH tests catch NIH bugs).

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"

echo "=== silex External Test Suite Fetcher ==="
echo "This will download 10 external test suites (~500MB)"
echo "Location: $REPOS_DIR"
echo ""

mkdir -p "$REPOS_DIR"
cd "$REPOS_DIR"

# Progress counter
SUITE=0
TOTAL=10

fetch_repo() {
    SUITE=$((SUITE + 1))
    local name="$1"
    local url="$2"
    local dir="$3"

    echo "[$SUITE/$TOTAL] Fetching $name..."

    if [ -d "$dir/.git" ]; then
        echo "    Already exists, updating..."
        (cd "$dir" && git pull --depth 1) || echo "    Update failed (non-fatal)"
    else
        git clone --depth 1 "$url" "$dir" 2>&1 | sed 's/^/    /'
    fi

    echo "    Done."
    echo ""
}

# SUITE 1: Oils/OSH spec tests (~1500 POSIX shell tests)
# The most comprehensive shell spec test suite, covering POSIX corner cases
fetch_repo "Oils/OSH" \
    "https://github.com/oilshell/oil.git" \
    "oil"

# SUITE 2: Smoosh (157 tests from formal mechanized semantics)
# Tests derived from a formal Coq model of POSIX shell semantics
fetch_repo "Smoosh" \
    "https://github.com/mgree/smoosh.git" \
    "smoosh"

# SUITE 3: modernish (shell feature detection + bug catalogue)
# Comprehensive bug database with FTL (fatal) / BUG / QRK (quirk) classifications
# CRITICAL REQUIREMENT: FTL count must be 0
fetch_repo "modernish" \
    "https://github.com/modernish/modernish.git" \
    "modernish"

# SUITE 4: mksh test suite (MirBSD Korn Shell regression tests)
# Mature test suite from a production shell implementation
fetch_repo "mksh" \
    "https://github.com/MirBSD/mksh.git" \
    "mksh"

# SUITE 5: GNU coreutils test suite (645 tests)
# The canonical test suite for Unix utilities (cat, cp, mv, ls, sort, wc, etc.)
# This is what uutils/coreutils uses for validation
fetch_repo "GNU coreutils" \
    "https://git.savannah.gnu.org/git/coreutils.git" \
    "coreutils"

# SUITE 6: GNU grep test suite (200+ tests)
# Authoritative tests for grep behavior (pattern matching, context, regex)
fetch_repo "GNU grep" \
    "https://git.savannah.gnu.org/git/grep.git" \
    "grep"

# SUITE 7: GNU sed test suite (100+ tests)
# Reference implementation tests for stream editing
fetch_repo "GNU sed" \
    "https://git.savannah.gnu.org/git/sed.git" \
    "sed"

# SUITE 8: toybox test suite
# Upstream test suite (silex is a toybox fork, so high compatibility expected)
fetch_repo "toybox" \
    "https://github.com/landley/toybox.git" \
    "toybox"

# SUITE 9: ShellSpec (BDD testing framework written in POSIX sh)
# Meta-test: If ShellSpec runs on silex, it validates shell correctness
fetch_repo "ShellSpec" \
    "https://github.com/shellspec/shellspec.git" \
    "shellspec"

# SUITE 10: Autoconf configure scripts (real-world validation)
# Major projects: curl, CPython, OpenSSL, SQLite, zlib
# CRITICAL REQUIREMENT: All configure scripts must pass (100%)
echo "[$((SUITE + 1))/$TOTAL] Fetching Autoconf test projects..."
mkdir -p projects
cd projects

fetch_repo "curl" \
    "https://github.com/curl/curl.git" \
    "curl"

fetch_repo "CPython" \
    "https://github.com/python/cpython.git" \
    "cpython"

fetch_repo "OpenSSL" \
    "https://github.com/openssl/openssl.git" \
    "openssl"

fetch_repo "SQLite" \
    "https://github.com/sqlite/sqlite.git" \
    "sqlite"

fetch_repo "zlib" \
    "https://github.com/madler/zlib.git" \
    "zlib"

cd "$REPOS_DIR"

echo "=== Fetch Complete ==="
echo ""
echo "Total disk usage:"
du -sh "$REPOS_DIR" 2>/dev/null || echo "(du unavailable)"
echo ""
echo "Next steps:"
echo "  1. Run individual suite: tests/external/run-oils-spec.sh"
echo "  2. Run all suites:       tests/external/run-all.sh"
echo "  3. Via Makefile:         make external-test"
echo ""
