#!/bin/sh
# tests/stress/run_ulimit_stress.sh — run tests under resource limits

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0

check() {
    local desc="$1"
    local rc="$2"
    if [ "$rc" -eq 0 ]; then
        echo "PASS: $desc"
        PASS=$(( PASS + 1 ))
    else
        echo "FAIL: $desc (exit $rc)"
        FAIL=$(( FAIL + 1 ))
    fi
}

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found at $MATCHBOX"
    exit 1
fi

# Test: sort under low FD limit (32 open files)
TMPFILE=$(mktemp)
printf 'c\na\nb\n' > "$TMPFILE"
RC=0
( ulimit -n 32 2>/dev/null; "$MATCHBOX" sort "$TMPFILE" > /dev/null 2>&1 ) || RC=$?
check "sort under ulimit -n 32" "$RC"
rm -f "$TMPFILE"

# Test: grep under low FD limit
TMPFILE=$(mktemp)
printf 'hello\nworld\n' > "$TMPFILE"
RC=0
( ulimit -n 32 2>/dev/null; "$MATCHBOX" grep hello "$TMPFILE" > /dev/null 2>&1 ) || RC=$?
check "grep under ulimit -n 32" "$RC"
rm -f "$TMPFILE"

# Test: cat under low FD limit
TMPFILE=$(mktemp)
printf 'test content\n' > "$TMPFILE"
RC=0
( ulimit -n 32 2>/dev/null; "$MATCHBOX" cat "$TMPFILE" > /dev/null 2>&1 ) || RC=$?
check "cat under ulimit -n 32" "$RC"
rm -f "$TMPFILE"

# Test: echo doesn't need files
RC=0
( ulimit -n 32 2>/dev/null; "$MATCHBOX" echo hello > /dev/null 2>&1 ) || RC=$?
check "echo under ulimit -n 32" "$RC"

# Test: wc on small file under FD limit
TMPFILE=$(mktemp)
printf 'one two three\n' > "$TMPFILE"
RC=0
( ulimit -n 32 2>/dev/null; "$MATCHBOX" wc -w "$TMPFILE" > /dev/null 2>&1 ) || RC=$?
check "wc under ulimit -n 32" "$RC"
rm -f "$TMPFILE"

echo ""
echo "ulimit stress: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
