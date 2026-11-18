#!/bin/sh
# tests/security/test_path_traversal.sh — path security tests for silex
# chmod +x tests/security/test_path_traversal.sh
# Usage: ./test_path_traversal.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
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

# We test via the silex tools themselves — e.g., mkdir and cat must not
# escape the working area due to /../ components in arguments.

# `..` is resolved by the kernel, component by component, exactly as POSIX
# pathname resolution says -- including `..` at / staying at /. That is the
# CORRECT behaviour for mkdir (GNU and busybox do the same), so these checks
# assert it, rather than asserting that a path cannot reach a directory it
# names. They are built so that every path they resolve -- however far up it
# climbs -- comes back down into $TMPDIR_PATH: the test never creates anything
# outside its own temp dir. (The old version ran
# `mkdir -p "$DEEP/../../../../../../../../../etc/injected"` and treated a
# correct mkdir as a failure, after creating /etc/injected.)

# .. resolves against the real parent: safe/subdir/../../injected is
# $TMPDIR_PATH/injected, and nothing is created anywhere else.
mkdir -p "$TMPDIR_PATH/safe/subdir"
"$SILEX" mkdir -p "$TMPDIR_PATH/safe/subdir/../../injected" > /dev/null 2>&1
check_exit_zero "mkdir /../ traversal: exits cleanly" "$?"
if [ -d "$TMPDIR_PATH/injected" ] && [ ! -e "$TMPDIR_PATH/safe/injected" ] &&
   [ ! -e "$TMPDIR_PATH/safe/subdir/injected" ]; then
    echo "PASS: mkdir /../: creates the directory at the resolved path"
    PASS=$((PASS + 1))
else
    echo "FAIL: mkdir /../: directory not created at \$TMPDIR_PATH/injected"
    FAIL=$((FAIL + 1))
fi

# Climb all the way to / -- more `..` than there are components, so the extra
# ones are resolved AT / -- and walk back down into the temp dir. The result
# must be exactly $TMPDIR_PATH/rooted: .. at / is /, not an error and not
# somewhere else.
DEEP="$TMPDIR_PATH/a/b/c/d/e"
mkdir -p "$DEEP"
REAL_TMP=$(cd "$TMPDIR_PATH" && pwd -P)
UP=""
n=0
depth=$(printf '%s\n' "$DEEP" | tr -cd '/' | wc -c)
while [ "$n" -lt $((depth + 8)) ]; do UP="$UP../"; n=$((n + 1)); done
"$SILEX" mkdir -p "$DEEP/$UP${REAL_TMP#/}/rooted" > /dev/null 2>&1
check_exit_zero "mkdir .. past /: exits cleanly" "$?"
if [ -d "$TMPDIR_PATH/rooted" ]; then
    echo "PASS: mkdir .. past /: .. at / resolves to / (back inside tmpdir)"
    PASS=$((PASS + 1))
else
    echo "FAIL: mkdir .. past /: \$TMPDIR_PATH/rooted not created"
    FAIL=$((FAIL + 1))
fi
# ...and only there: nothing was created under the intermediate components.
if [ -z "$(find "$TMPDIR_PATH/a" -name rooted 2>/dev/null)" ]; then
    echo "PASS: mkdir .. past /: nothing created along the way"
    PASS=$((PASS + 1))
else
    echo "FAIL: mkdir .. past /: stray directory under \$TMPDIR_PATH/a"
    FAIL=$((FAIL + 1))
fi

# A `..` through a symlink resolves against the symlink's TARGET (physical
# resolution, as the kernel does it): lnk -> a/b/c, so lnk/../viasym is
# a/b/viasym, not $TMPDIR_PATH/viasym.
ln -s "$TMPDIR_PATH/a/b/c" "$TMPDIR_PATH/lnk"
"$SILEX" mkdir -p "$TMPDIR_PATH/lnk/../viasym" > /dev/null 2>&1
if [ -d "$TMPDIR_PATH/a/b/viasym" ] && [ ! -e "$TMPDIR_PATH/viasym" ]; then
    echo "PASS: mkdir symlink/..: resolved physically (against the target)"
    PASS=$((PASS + 1))
else
    echo "FAIL: mkdir symlink/..: not created at the physical parent"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# Filenames starting with - (dash): tools must handle safely with --
# ===========================================================================

DASHFILE="$TMPDIR_PATH/-dangerous-file"
printf 'test content\n' > "$DASHFILE"

# cat -- -dangerous-file should read the file, not interpret as flag
got=$("$SILEX" cat -- "$DASHFILE" 2>/dev/null)
check "cat -- -filename: reads file, not flag" "$got" "test content"

# cp -- -source -dest
DASHDEST="$TMPDIR_PATH/-dest-file"
"$SILEX" cp -- "$DASHFILE" "$DASHDEST" > /dev/null 2>&1
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
"$SILEX" mkdir -- "$DASHDIR" > /dev/null 2>&1
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
    got=$("$SILEX" cat -- "$SEMICOLONFILE" 2>/dev/null)
    check "cat file with semicolon in name: no injection" "$got" "safe content"
fi

# File with backtick in name
BACKTICKFILE="$TMPDIR_PATH/file\`echo INJECTED\`"
printf 'safe content\n' > "$BACKTICKFILE" 2>/dev/null || true
if [ -f "$BACKTICKFILE" ]; then
    got=$("$SILEX" cat -- "$BACKTICKFILE" 2>/dev/null)
    check "cat file with backtick in name: no injection" "$got" "safe content"
fi

# File with dollar sign in name
DOLLARFILE="$TMPDIR_PATH/file\$HOME"
printf 'safe content\n' > "$DOLLARFILE" 2>/dev/null || true
if [ -f "$DOLLARFILE" ]; then
    got=$("$SILEX" cat -- "$DOLLARFILE" 2>/dev/null)
    check "cat file with \$ in name: no variable expansion" "$got" "safe content"
fi

# File with newline in name (unusual but possible on Linux)
NEWLINEFILE="$TMPDIR_PATH/$(printf 'file\nwith\nnewlines')"
printf 'newline file content\n' > "$NEWLINEFILE" 2>/dev/null || true
if [ -f "$NEWLINEFILE" ]; then
    got=$("$SILEX" cat -- "$NEWLINEFILE" 2>/dev/null)
    check "cat file with newline in name: content correct" "$got" "newline file content"
fi

# File with space in name (injection attempt via unquoted expansion)
SPACEINJ="$TMPDIR_PATH/a b; echo INJECTED"
printf 'space file\n' > "$SPACEINJ" 2>/dev/null || true
if [ -f "$SPACEINJ" ]; then
    got=$("$SILEX" cat -- "$SPACEINJ" 2>/dev/null)
    check "cat file with space+semicolon: no injection" "$got" "space file"
fi

# ===========================================================================
# `..` after a missing component is not a lexical shortcut
# ===========================================================================

# A path through a component that does not exist fails cleanly, however many
# `..` follow it: x is missing, so x/../inside.txt is an error (ENOENT), not
# a lexical shortcut to $TMPDIR_PATH/inside.txt.
printf 'inside\n' > "$TMPDIR_PATH/inside.txt"
"$SILEX" cat "$TMPDIR_PATH/x/../inside.txt" > /dev/null 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "PASS: cat through a missing component: rejected cleanly"
    PASS=$((PASS + 1))
else
    echo "FAIL: cat through a missing component: succeeded (lexical .. resolution)"
    FAIL=$((FAIL + 1))
fi
# ...while the same climb through components that DO exist reads the file,
# again without ever naming anything outside the temp dir.
got=$("$SILEX" cat "$DEEP/$UP${REAL_TMP#/}/inside.txt" 2>/dev/null)
check "cat .. past / and back down: reads the file" "$got" "inside"

# ===========================================================================
# Null byte in path (should be rejected or handled safely)
# ===========================================================================

# Most shells can't pass null bytes, but we verify the tool doesn't crash
# by testing with a path that ends with a weird character
WEIRDFILE="$TMPDIR_PATH/test_weird_"
printf 'weird\n' > "${WEIRDFILE}name"
got=$("$SILEX" cat -- "${WEIRDFILE}name" 2>/dev/null)
check "cat file with trailing underscore in name" "$got" "weird"

echo ""
echo "path traversal tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
