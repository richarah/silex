#!/bin/sh
# tests/edge/test_long_lines.sh — long line edge cases

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

# Generate a 1MB line (1024*1024 'A' chars, no newline, then newline)
BIGFILE="$TMPDIR_EDGE/bigline.txt"
python3 -c "import sys; sys.stdout.write('A' * (1024*1024) + '\n')" > "$BIGFILE" 2>/dev/null \
    || perl -e "print 'A' x (1024*1024), '\n'" > "$BIGFILE" 2>/dev/null \
    || dd if=/dev/zero bs=1048576 count=1 2>/dev/null | tr '\0' 'A' > "$BIGFILE"
printf '\n' >> "$BIGFILE"

# cat must not crash
"$SILEX" cat "$BIGFILE" > /dev/null 2>&1
check "cat: 1MB line doesn't crash" "$?"

# wc must not crash
"$SILEX" wc -l "$BIGFILE" > /dev/null 2>&1
check "wc: 1MB line doesn't crash" "$?"

"$SILEX" wc -c "$BIGFILE" > /dev/null 2>&1
check "wc -c: 1MB line doesn't crash" "$?"

# grep must not crash (pattern found)
"$SILEX" grep 'A' "$BIGFILE" > /dev/null 2>&1
check "grep: 1MB line doesn't crash (match)" "$?"

# grep must not crash (pattern not found), exit 1 is OK
RC=$("$SILEX" grep 'Z' "$BIGFILE" > /dev/null 2>&1; echo $?)
# We don't care about rc here — just that it doesn't crash with signal
if [ "$RC" -lt 128 ]; then
    check "grep: 1MB line no match doesn't crash" "0"
else
    check "grep: 1MB line no match doesn't crash" "1"
fi

# sort must not crash
"$SILEX" sort "$BIGFILE" > /dev/null 2>&1
check "sort: 1MB line doesn't crash" "$?"

# head must not crash
"$SILEX" head -n 1 "$BIGFILE" > /dev/null 2>&1
check "head: 1MB line doesn't crash" "$?"

# tail must not crash
"$SILEX" tail -n 1 "$BIGFILE" > /dev/null 2>&1
check "tail: 1MB line doesn't crash" "$?"

# 64KB line (near buffer boundaries for many tools)
MEDFILE="$TMPDIR_EDGE/medline.txt"
printf '%0.s-' $(seq 1 65536) > "$MEDFILE"
printf '\n' >> "$MEDFILE"

"$SILEX" cat "$MEDFILE" > /dev/null 2>&1
check "cat: 64KB line doesn't crash" "$?"

"$SILEX" grep '-' "$MEDFILE" > /dev/null 2>&1
check "grep: 64KB line doesn't crash" "$?"

# Many short lines (1 million lines)
MANYFILE="$TMPDIR_EDGE/manylines.txt"
seq 1 1000000 > "$MANYFILE"

"$SILEX" wc -l "$MANYFILE" > /dev/null 2>&1
check "wc: 1M lines doesn't crash" "$?"

"$SILEX" sort -n "$MANYFILE" > /dev/null 2>&1
check "sort: 1M lines doesn't crash" "$?"

echo ""
echo "long line edge tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
