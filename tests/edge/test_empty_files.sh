#!/bin/sh
# tests/edge/test_empty_files.sh — empty and single-byte input edge cases

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0
TMPDIR_EDGE=$(mktemp -d)
trap 'rm -rf "$TMPDIR_EDGE"' EXIT INT TERM

check() {
    local desc="$1"
    local got="$2"
    local expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"
        PASS=$(( PASS + 1 ))
    else
        echo "FAIL: $desc"
        echo "  expected: |$(printf '%s' "$expected" | cat -v)|"
        echo "  got:      |$(printf '%s' "$got" | cat -v)|"
        FAIL=$(( FAIL + 1 ))
    fi
}

check_rc() {
    local desc="$1"
    local got_rc="$2"
    local expected_rc="$3"
    if [ "$got_rc" = "$expected_rc" ]; then
        echo "PASS: $desc"
        PASS=$(( PASS + 1 ))
    else
        echo "FAIL: $desc (expected rc=$expected_rc, got rc=$got_rc)"
        FAIL=$(( FAIL + 1 ))
    fi
}

EMPTYFILE="$TMPDIR_EDGE/empty.txt"
: > "$EMPTYFILE"

# cat empty file: produces no output
got=$("$MATCHBOX" cat "$EMPTYFILE")
check "cat: empty file" "$got" ""

# wc empty file: 0 0 0
got=$("$MATCHBOX" wc -lwc "$EMPTYFILE" | tr -s ' ' | sed 's/^ //')
# Expected: "0 0 0 path"
case "$got" in
    "0 0 0"*) check "wc: empty file (0 0 0)" "ok" "ok" ;;
    *)        check "wc: empty file (0 0 0)" "$got" "0 0 0 ..." ;;
esac

# grep on empty file: no match, exit 1
"$MATCHBOX" grep 'anything' "$EMPTYFILE" >/dev/null 2>&1
check_rc "grep: empty file, no match" "$?" "1"

# sort empty file: produces no output
got=$("$MATCHBOX" sort "$EMPTYFILE")
check "sort: empty file" "$got" ""

# sed on empty file: produces no output
got=$("$MATCHBOX" sed 's/x/y/' "$EMPTYFILE")
check "sed: empty file" "$got" ""

# head empty file: no output
got=$("$MATCHBOX" head -n 1 "$EMPTYFILE")
check "head: empty file" "$got" ""

# tail empty file: no output
got=$("$MATCHBOX" tail -n 1 "$EMPTYFILE")
check "tail: empty file" "$got" ""

# File with no trailing newline
NONLFILE="$TMPDIR_EDGE/nonewline.txt"
printf 'no newline' > "$NONLFILE"

got=$("$MATCHBOX" cat "$NONLFILE")
check "cat: file without trailing newline" "$got" "no newline"

got=$("$MATCHBOX" wc -l "$NONLFILE" | tr -s ' ' | sed 's/^ //')
case "$got" in
    "0"*) check "wc -l: file without trailing newline (0 lines)" "ok" "ok" ;;
    *)    check "wc -l: file without trailing newline (0 lines)" "$got" "0 ..." ;;
esac

got=$("$MATCHBOX" grep 'no' "$NONLFILE")
check "grep: file without trailing newline" "$got" "no newline"

# Single-byte file
SBFILE="$TMPDIR_EDGE/singlebyte.txt"
printf 'x' > "$SBFILE"
got=$("$MATCHBOX" cat "$SBFILE")
check "cat: single-byte file" "$got" "x"

got=$("$MATCHBOX" wc -c "$SBFILE" | tr -s ' ' | sed 's/^ //')
case "$got" in
    "1"*) check "wc -c: single-byte file" "ok" "ok" ;;
    *)    check "wc -c: single-byte file" "$got" "1 ..." ;;
esac

echo ""
echo "empty file edge tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
