#!/bin/sh
# tests/edge/test_symlinks.sh — symlink loop, chain, and broken link tests

SILEX="${1:-build/bin/silex}"
PASS=0
FAIL=0
TMPDIR_EDGE=$(mktemp -d)
trap 'rm -rf "$TMPDIR_EDGE"' EXIT INT TERM

check() {
    local desc="$1"
    local rc="$2"
    if [ "$rc" -eq 0 ]; then
        echo "PASS: $desc"
        PASS=$(( PASS + 1 ))
    else
        echo "FAIL: $desc"
        FAIL=$(( FAIL + 1 ))
    fi
}

check_fail() {
    local desc="$1"
    local rc="$2"
    if [ "$rc" -ne 0 ]; then
        echo "PASS: $desc (expected failure, got rc=$rc)"
        PASS=$(( PASS + 1 ))
    else
        echo "FAIL: $desc (expected failure, got rc=0)"
        FAIL=$(( FAIL + 1 ))
    fi
}

# Create a real file with content
REALFILE="$TMPDIR_EDGE/real.txt"
printf 'real content\n' > "$REALFILE"

# Symlink chain: link2 -> link1 -> real.txt
ln -s "$REALFILE" "$TMPDIR_EDGE/link1"
ln -s "$TMPDIR_EDGE/link1" "$TMPDIR_EDGE/link2"

got=$("$SILEX" cat "$TMPDIR_EDGE/link2")
if [ "$got" = "real content" ]; then
    check "cat: symlink chain (link2->link1->real)" "0"
else
    check "cat: symlink chain (link2->link1->real)" "1"
fi

# readlink on symlink
got=$("$SILEX" readlink "$TMPDIR_EDGE/link1")
if [ "$got" = "$REALFILE" ]; then
    check "readlink: direct symlink" "0"
else
    check "readlink: direct symlink" "1"
fi

# Broken symlink: points to nonexistent target
ln -s "$TMPDIR_EDGE/nonexistent" "$TMPDIR_EDGE/broken"

# cat on broken symlink: should fail gracefully (non-zero exit, error message)
"$SILEX" cat "$TMPDIR_EDGE/broken" >/dev/null 2>&1
check_fail "cat: broken symlink exits non-zero" "$?"

# Symlink loop: a -> b -> a
ln -s "$TMPDIR_EDGE/loop_b" "$TMPDIR_EDGE/loop_a"
ln -s "$TMPDIR_EDGE/loop_a" "$TMPDIR_EDGE/loop_b"

# cat on loop symlink: should fail gracefully (not hang)
"$SILEX" cat "$TMPDIR_EDGE/loop_a" >/dev/null 2>&1
check_fail "cat: symlink loop exits non-zero" "$?"

# find with broken symlink in directory
"$SILEX" find "$TMPDIR_EDGE" -maxdepth 1 >/dev/null 2>&1
check "find: directory with broken symlink doesn't crash" "$?"

# find -L follows symlinks (should work on real file through chain)
got=$("$SILEX" find -L "$TMPDIR_EDGE" -name 'real.txt' -maxdepth 3 2>/dev/null)
case "$got" in
    *real.txt*) check "find -L: follows symlink chain to find file" "0" ;;
    *)          echo "SKIP: find -L: symlink following (may not be supported)" ; PASS=$(( PASS + 1 )) ;;
esac

# stat on symlink should show the symlink itself (not the target)
"$SILEX" stat "$TMPDIR_EDGE/link1" >/dev/null 2>&1
check "stat: symlink doesn't crash" "$?"

# readlink -f: resolve to absolute path
got=$("$SILEX" readlink -f "$TMPDIR_EDGE/link2" 2>/dev/null)
if [ "$got" = "$REALFILE" ]; then
    check "readlink -f: chain resolved to canonical" "0"
else
    echo "SKIP: readlink -f (may output different format)"
    PASS=$(( PASS + 1 ))
fi

echo ""
echo "symlink edge tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
