#!/bin/sh
# tests/edge/test_binary.sh — binary content and null byte edge cases

MATCHBOX="${1:-build/bin/matchbox}"
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
        echo "FAIL: $desc (unexpected crash or error)"
        FAIL=$(( FAIL + 1 ))
    fi
}

# Create binary file with null bytes and high bytes
BINFILE="$TMPDIR_EDGE/binary.bin"
printf '\x00\x01\x02\x03\xff\xfe\xfd\x00\n' > "$BINFILE"

# cat should not crash on binary
"$MATCHBOX" cat "$BINFILE" > /dev/null 2>&1
check "cat: binary file doesn't crash" "$?"

# wc should not crash on binary
"$MATCHBOX" wc -c "$BINFILE" > /dev/null 2>&1
check "wc -c: binary file doesn't crash" "$?"

# grep should not crash on binary (BRE pattern)
"$MATCHBOX" grep -a 'x' "$BINFILE" > /dev/null 2>&1
RC=$?
# rc can be 0 or 1 (no match); crash would be >=128 or signal
if [ "$RC" -lt 128 ]; then
    check "grep: binary file doesn't crash" "0"
else
    check "grep: binary file doesn't crash" "1"
fi

# File with only null bytes (no newlines)
NULLFILE="$TMPDIR_EDGE/nulls.bin"
printf '\x00\x00\x00\x00\x00' > "$NULLFILE"

"$MATCHBOX" cat "$NULLFILE" > /dev/null 2>&1
check "cat: null-only file doesn't crash" "$?"

"$MATCHBOX" wc -c "$NULLFILE" > /dev/null 2>&1
check "wc -c: null-only file doesn't crash" "$?"

# File with mix of binary and text
MIXFILE="$TMPDIR_EDGE/mixed.txt"
printf 'text line\n\x00\x01binary\x00line\nmore text\n' > "$MIXFILE"

"$MATCHBOX" cat "$MIXFILE" > /dev/null 2>&1
check "cat: mixed binary/text file doesn't crash" "$?"

"$MATCHBOX" grep 'text' "$MIXFILE" > /dev/null 2>&1
check "grep: mixed binary/text file doesn't crash" "$?"

# sed on binary: should not crash
"$MATCHBOX" sed 's/text/TEXT/' "$MIXFILE" > /dev/null 2>&1
check "sed: mixed binary/text file doesn't crash" "$?"

# Large binary file
LARGEFILE="$TMPDIR_EDGE/large.bin"
dd if=/dev/urandom of="$LARGEFILE" bs=65536 count=4 2>/dev/null || \
    head -c 262144 /dev/urandom > "$LARGEFILE" 2>/dev/null || \
    printf '\xff\x00\xaa\x55%.0s' $(seq 1 65536) > "$LARGEFILE"

"$MATCHBOX" cat "$LARGEFILE" > /dev/null 2>&1
check "cat: 256KB random binary doesn't crash" "$?"

"$MATCHBOX" wc -c "$LARGEFILE" > /dev/null 2>&1
check "wc -c: 256KB random binary doesn't crash" "$?"

echo ""
echo "binary edge tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
