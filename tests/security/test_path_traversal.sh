#!/bin/sh
# tests/security/test_path_traversal.sh — path security tests for matchbox
# chmod +x tests/security/test_path_traversal.sh
# Usage: ./test_path_traversal.sh [path/to/matchbox]

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0

check() {
    local desc="$1"
    local got="$2"
    local expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected: '$expected'"
        echo "  got:      '$got'"
        FAIL=$((FAIL + 1))
    fi
}

check_exit_nonzero() {
    local desc="$1"
    local got_exit="$2"
    if [ "$got_exit" -ne 0 ]; then
        echo "PASS: $desc (exit $got_exit)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected non-zero, got 0)"
        FAIL=$((FAIL + 1))
    fi
}

check_exit_zero() {
    local desc="$1"
    local got_exit="$2"
    if [ "$got_exit" -eq 0 ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected 0, got $got_exit)"
        FAIL=$((FAIL + 1))
    fi
}

check_no_escape() {
    local desc="$1"
    local path="$2"
    local sensitive_dir="$3"
    # Verify that the path does not resolve to or contain the sensitive dir
    RESOLVED=$(cd "$(dirname "$path")" 2>/dev/null && pwd -P 2>/dev/null || echo "")
    if printf '%s' "$RESOLVED" | grep -q "^${sensitive_dir}"; then
        echo "FAIL: $desc (path escaped to $RESOLVED)"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: $desc (path confined)"
        PASS=$((PASS + 1))
    fi
}

TMPDIR_PATH=$(mktemp -d)
trap 'rm -rf "$TMPDIR_PATH"' EXIT INT TERM

# ===========================================================================
# path_normalize: /../ handling
# ===========================================================================

# We test via the matchbox tools themselves — e.g., mkdir and cat must not
# escape the working area due to /../ components in arguments.

# mkdir with /../ traversal should stay within tmpdir or fail
mkdir -p "$TMPDIR_PATH/safe/subdir"
"$MATCHBOX" mkdir -p "$TMPDIR_PATH/safe/subdir/../../injected" > /dev/null 2>&1
check_exit_zero "mkdir /../ traversal: exits cleanly" "$?"
# The resolved path should be $TMPDIR_PATH/safe/injected — still inside TMPDIR_PATH
if [ -d "$TMPDIR_PATH/safe/injected" ] || [ -d "$TMPDIR_PATH/injected" ]; then
    echo "PASS: mkdir /../: creates directory at normalised path (within tmpdir)"
    PASS=$((PASS + 1))
elif [ -d "/injected" ]; then
    echo "FAIL: mkdir /../: escaped to /injected (filesystem root!)"
    FAIL=$((FAIL + 1))
else
    echo "PASS: mkdir /../: did not escape to filesystem root"
    PASS=$((PASS + 1))
fi

# Attempt path traversal above TMPDIR root — should not reach /tmp/.. = /
DEEP="$TMPDIR_PATH/a/b/c/d/e"
mkdir -p "$DEEP"
"$MATCHBOX" mkdir -p "$DEEP/../../../../../../../../../etc/injected" > /dev/null 2>&1
if [ -d "/etc/injected" ]; then
    echo "FAIL: path traversal: escaped to /etc/injected"
    FAIL=$((FAIL + 1))
else
    echo "PASS: path traversal: did not create /etc/injected"
    PASS=$((PASS + 1))
fi

# ===========================================================================
# Filenames starting with - (dash): tools must handle safely with --
# ===========================================================================

DASHFILE="$TMPDIR_PATH/-dangerous-file"
printf 'test content\n' > "$DASHFILE"

# cat -- -dangerous-file should read the file, not interpret as flag
got=$("$MATCHBOX" cat -- "$DASHFILE" 2>/dev/null)
check "cat -- -filename: reads file, not flag" "$got" "test content"

# cp -- -source -dest
DASHDEST="$TMPDIR_PATH/-dest-file"
"$MATCHBOX" cp -- "$DASHFILE" "$DASHDEST" > /dev/null 2>&1
check_exit_zero "cp -- -src -dst: exit 0" "$?"
if [ -f "$DASHDEST" ]; then
    echo "PASS: cp -- -src -dst: destination created"
    PASS=$((PASS + 1))
else
    echo "FAIL: cp -- -src -dst: destination not created"
    FAIL=$((FAIL + 1))
fi

# mkdir -- -dashdir
DASHDIR="$TMPDIR_PATH/-dashdir"
"$MATCHBOX" mkdir -- "$DASHDIR" > /dev/null 2>&1
check_exit_zero "mkdir -- -dashdir: exit 0" "$?"
if [ -d "$DASHDIR" ]; then
    echo "PASS: mkdir -- -dashdir: directory created"
    PASS=$((PASS + 1))
else
    echo "FAIL: mkdir -- -dashdir: directory not created"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# Filenames with special characters: no shell injection
# ===========================================================================

# File with semicolon in name
SEMICOLONFILE="$TMPDIR_PATH/file;echo INJECTED"
printf 'safe content\n' > "$SEMICOLONFILE" 2>/dev/null || true
if [ -f "$SEMICOLONFILE" ]; then
    got=$("$MATCHBOX" cat -- "$SEMICOLONFILE" 2>/dev/null)
    check "cat file with semicolon in name: no injection" "$got" "safe content"
fi

# File with backtick in name
BACKTICKFILE="$TMPDIR_PATH/file\`echo INJECTED\`"
printf 'safe content\n' > "$BACKTICKFILE" 2>/dev/null || true
if [ -f "$BACKTICKFILE" ]; then
    got=$("$MATCHBOX" cat -- "$BACKTICKFILE" 2>/dev/null)
    check "cat file with backtick in name: no injection" "$got" "safe content"
fi

# File with dollar sign in name
DOLLARFILE="$TMPDIR_PATH/file\$HOME"
printf 'safe content\n' > "$DOLLARFILE" 2>/dev/null || true
if [ -f "$DOLLARFILE" ]; then
    got=$("$MATCHBOX" cat -- "$DOLLARFILE" 2>/dev/null)
    check "cat file with \$ in name: no variable expansion" "$got" "safe content"
fi

# File with newline in name (unusual but possible on Linux)
NEWLINEFILE="$TMPDIR_PATH/$(printf 'file\nwith\nnewlines')"
printf 'newline file content\n' > "$NEWLINEFILE" 2>/dev/null || true
if [ -f "$NEWLINEFILE" ]; then
    got=$("$MATCHBOX" cat -- "$NEWLINEFILE" 2>/dev/null)
    check "cat file with newline in name: content correct" "$got" "newline file content"
fi

# File with space in name (injection attempt via unquoted expansion)
SPACEINJ="$TMPDIR_PATH/a b; echo INJECTED"
printf 'space file\n' > "$SPACEINJ" 2>/dev/null || true
if [ -f "$SPACEINJ" ]; then
    got=$("$MATCHBOX" cat -- "$SPACEINJ" 2>/dev/null)
    check "cat file with space+semicolon: no injection" "$got" "space file"
fi

# ===========================================================================
# Verify path_normalize does not return path above anchor
# ===========================================================================

# Use matchbox --canon-path or similar if available, else test via mkdir behaviour
# Test: normalise /tmp/a/../../../etc -> must not be /etc
TRAV_PATH="$TMPDIR_PATH/x/../../../etc"
"$MATCHBOX" cat "$TRAV_PATH/passwd" > /dev/null 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "PASS: path traversal to /etc/passwd: rejected or not found cleanly"
    PASS=$((PASS + 1))
else
    echo "FAIL: path traversal to /etc/passwd: succeeded (unexpected)"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# Null byte in path (should be rejected or handled safely)
# ===========================================================================

# Most shells can't pass null bytes, but we verify the tool doesn't crash
# by testing with a path that ends with a weird character
WEIRDFILE="$TMPDIR_PATH/test_weird_"
printf 'weird\n' > "${WEIRDFILE}name"
got=$("$MATCHBOX" cat -- "${WEIRDFILE}name" 2>/dev/null)
check "cat file with trailing underscore in name" "$got" "weird"

echo ""
echo "path traversal tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
