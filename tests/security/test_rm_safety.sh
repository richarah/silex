#!/bin/sh
# tests/security/test_rm_safety.sh — rm safety tests for matchbox
# chmod +x tests/security/test_rm_safety.sh
# Usage: ./test_rm_safety.sh [path/to/matchbox]
#
# IMPORTANT: These tests must never actually damage the filesystem.
# All destructive tests verify rejection BEFORE any filesystem action occurs.

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0

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

check_exists() {
    local desc="$1"
    local path="$2"
    if [ -e "$path" ]; then
        echo "PASS: $desc (path exists)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (path was removed!)"
        FAIL=$((FAIL + 1))
    fi
}

TMPDIR_RM=$(mktemp -d)
trap 'rm -rf "$TMPDIR_RM"' EXIT INT TERM

# ===========================================================================
# rm -rf / must be rejected
# ===========================================================================

"$MATCHBOX" rm -rf / > /dev/null 2>&1
check_exit_nonzero "rm -rf /: rejected (non-zero exit)" "$?"

# Verify / still exists and is intact
check_exists "rm -rf /: / not deleted" "/"
check_exists "rm -rf /: /etc still present" "/etc"
check_exists "rm -rf /: /bin still present" "/bin"

# ===========================================================================
# rm --no-preserve-root must be rejected
# ===========================================================================

"$MATCHBOX" rm --no-preserve-root -rf / > /dev/null 2>&1
check_exit_nonzero "rm --no-preserve-root -rf /: rejected" "$?"
check_exists "rm --no-preserve-root: / still present" "/"

# ===========================================================================
# rm with symlink-to-/ in path
# ===========================================================================

SYMDIR="$TMPDIR_RM/rootlink"
ln -s / "$SYMDIR" 2>/dev/null || true
if [ -L "$SYMDIR" ]; then
    "$MATCHBOX" rm -rf "$SYMDIR/." > /dev/null 2>&1
    check_exit_nonzero "rm -rf <symlink-to-/>/.: rejected" "$?"
    check_exists "rm with symlink-to-/: / still intact" "/"
    check_exists "rm with symlink-to-/: /etc still intact" "/etc"
else
    echo "SKIP: could not create symlink to / (permissions?)"
fi

# ===========================================================================
# rm -f nonexistent_file must succeed (exit 0)
# ===========================================================================

"$MATCHBOX" rm -f "$TMPDIR_RM/does_not_exist_xyz" > /dev/null 2>&1
check_exit_zero "rm -f nonexistent: exit 0 (-f suppresses error)" "$?"

"$MATCHBOX" rm -f "$TMPDIR_RM/does_not_exist_xyz" "$TMPDIR_RM/also_missing" > /dev/null 2>&1
check_exit_zero "rm -f multiple nonexistent files: exit 0" "$?"

# rm without -f on nonexistent: should fail
"$MATCHBOX" rm "$TMPDIR_RM/does_not_exist_xyz" > /dev/null 2>&1
check_exit_nonzero "rm nonexistent without -f: non-zero exit" "$?"

# ===========================================================================
# rm of file with spaces in name
# ===========================================================================

SPACEFILE="$TMPDIR_RM/file with spaces.txt"
printf 'content\n' > "$SPACEFILE"
"$MATCHBOX" rm "$SPACEFILE" > /dev/null 2>&1
check_exit_zero "rm file with spaces: exit 0" "$?"
if [ ! -e "$SPACEFILE" ]; then
    echo "PASS: rm file with spaces: file deleted"
    PASS=$((PASS + 1))
else
    echo "FAIL: rm file with spaces: file still exists"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# rm with -- separator for files starting with -
# ===========================================================================

DASHFILE="$TMPDIR_RM/-starts-with-dash"
printf 'content\n' > "$DASHFILE"

# Without -- : rm would interpret it as a flag (should fail or error)
"$MATCHBOX" rm "$DASHFILE" > /dev/null 2>&1
GOT_WITHOUT=$?

# With -- : must succeed
printf 'content\n' > "$DASHFILE"
"$MATCHBOX" rm -- "$DASHFILE" > /dev/null 2>&1
check_exit_zero "rm -- -starts-with-dash: exit 0 with -- separator" "$?"
if [ ! -e "$DASHFILE" ]; then
    echo "PASS: rm --: file deleted correctly"
    PASS=$((PASS + 1))
else
    echo "FAIL: rm --: file was not deleted"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# rm of a regular file (sanity: must work)
# ===========================================================================

NORMAL="$TMPDIR_RM/normal_file.txt"
printf 'hello\n' > "$NORMAL"
"$MATCHBOX" rm "$NORMAL" > /dev/null 2>&1
check_exit_zero "rm normal file: exit 0" "$?"
if [ ! -e "$NORMAL" ]; then
    echo "PASS: rm normal file: file deleted"
    PASS=$((PASS + 1))
else
    echo "FAIL: rm normal file: file not deleted"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# rm -r on directory
# ===========================================================================

RMDIR="$TMPDIR_RM/subdir_to_remove"
mkdir -p "$RMDIR/nested"
printf 'x\n' > "$RMDIR/nested/file.txt"
"$MATCHBOX" rm -r "$RMDIR" > /dev/null 2>&1
check_exit_zero "rm -r directory: exit 0" "$?"
if [ ! -e "$RMDIR" ]; then
    echo "PASS: rm -r directory: directory removed"
    PASS=$((PASS + 1))
else
    echo "FAIL: rm -r directory: directory still exists"
    FAIL=$((FAIL + 1))
fi

# ===========================================================================
# rm of path resolving to / via .. traversal
# ===========================================================================

# $TMPDIR_RM is /tmp/XXXX (2 levels from /), so ../../ goes: XXXX -> /tmp -> /
"$MATCHBOX" rm -rf "$TMPDIR_RM/../.." > /dev/null 2>&1
check_exit_nonzero "rm -rf path/../..: rejected when resolves to /" "$?"
check_exists "rm via ..: / still intact" "/"

echo ""
echo "rm safety tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
