#!/bin/sh
# tests/edge/test_limits.sh — PATH_MAX filenames, /dev/null, /dev/zero

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

check_output() {
    local desc="$1"
    local got="$2"
    local expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"
        PASS=$(( PASS + 1 ))
    else
        echo "FAIL: $desc"
        echo "  expected: |$expected|"
        echo "  got:      |$got|"
        FAIL=$(( FAIL + 1 ))
    fi
}

# /dev/null as input
got=$("$SILEX" cat /dev/null)
check_output "cat /dev/null: empty output" "$got" ""

got=$("$SILEX" wc -l /dev/null | tr -s ' ' | sed 's/^ //')
case "$got" in
    "0"*) check "wc -l /dev/null: 0 lines" "0" ;;
    *)    check "wc -l /dev/null: 0 lines" "1" ;;
esac

"$SILEX" grep 'anything' /dev/null >/dev/null 2>&1
RC=$?
if [ "$RC" -eq 1 ]; then
    check "grep /dev/null: exit 1 (no match)" "0"
else
    check "grep /dev/null: exit 1 (no match)" "1"
fi

"$SILEX" sort /dev/null > /dev/null 2>&1
check "sort /dev/null: no crash" "$?"

"$SILEX" sed 's/x/y/' /dev/null > /dev/null 2>&1
check "sed /dev/null: no crash" "$?"

# /dev/zero as input — must NOT loop forever (bounded read via head)
# head -c 10 /dev/zero: get 10 bytes from /dev/zero
got=$(head -c 10 /dev/zero | "$SILEX" wc -c)
case "$got" in
    *10*) check "wc -c: /dev/zero bounded read via head" "0" ;;
    *)    check "wc -c: /dev/zero bounded read via head" "1" ;;
esac

# /dev/null as output
"$SILEX" echo hello > /dev/null 2>&1
check "echo to /dev/null: no crash" "$?"

printf 'test\n' | "$SILEX" sort > /dev/null 2>&1
check "sort stdout to /dev/null: no crash" "$?"

# Near PATH_MAX filenames
# Build a deeply nested directory with a long name
LONGDIR="$TMPDIR_EDGE"
DEPTH=0
while [ "$DEPTH" -lt 5 ] && [ ${#LONGDIR} -lt 200 ]; do
    LONGDIR="$LONGDIR/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    DEPTH=$(( DEPTH + 1 ))
done
mkdir -p "$LONGDIR" 2>/dev/null && {
    LONGFILE="$LONGDIR/testfile.txt"
    printf 'deep content\n' > "$LONGFILE"
    "$SILEX" cat "$LONGFILE" > /dev/null 2>&1
    check "cat: deep path (long filename)" "$?"
}

echo ""
echo "limits edge tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
