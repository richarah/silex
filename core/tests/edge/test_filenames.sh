#!/bin/sh
# tests/edge/test_filenames.sh — special filename edge cases

SILEX="${1:-build/bin/silex}"
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
        echo "  expected: $(printf '%s' "$expected" | cat -v)"
        echo "  got:      $(printf '%s' "$got" | cat -v)"
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

# Filename with spaces
SPACEFILE="$TMPDIR_EDGE/file with spaces.txt"
printf 'hello spaces\n' > "$SPACEFILE"
got=$("$SILEX" cat "$SPACEFILE")
check "cat: filename with spaces" "$got" "hello spaces"

# Filename with leading dash (use --)
DASHFILE="$TMPDIR_EDGE/-leading-dash.txt"
printf 'dash content\n' > "$DASHFILE"
got=$("$SILEX" cat -- "$DASHFILE")
check "cat: filename with leading dash (--)" "$got" "dash content"

# Filename with newline (if filesystem supports it)
# Most Linux filesystems support newlines in filenames
NLFILE="$TMPDIR_EDGE/file
with
newline.txt"
if printf 'newline name\n' > "$NLFILE" 2>/dev/null; then
    got=$("$SILEX" cat "$NLFILE")
    check "cat: filename with embedded newlines" "$got" "newline name"
fi

# Filename with special shell chars (no glob expansion needed since we quote)
SPECFILE="$TMPDIR_EDGE/file[special].txt"
printf 'special\n' > "$SPECFILE"
got=$("$SILEX" cat "$SPECFILE")
check "cat: filename with [] chars" "$got" "special"

# Filename with unicode characters
UNIFILE="$TMPDIR_EDGE/файл.txt"
printf 'unicode\n' > "$UNIFILE"
got=$("$SILEX" cat "$UNIFILE")
check "cat: unicode filename" "$got" "unicode"

# wc on file with spaces in name
got=$("$SILEX" wc -l "$SPACEFILE")
# wc output format: "N filename" — just check it includes 1
case "$got" in
    *1*) check "wc: filename with spaces" "ok" "ok" ;;
    *)   check "wc: filename with spaces" "fail" "ok" ;;
esac

# grep on file with leading dash
got=$("$SILEX" grep -- 'dash' -- "$DASHFILE")
check "grep: filename with leading dash" "$got" "dash content"

echo ""
echo "filename edge tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
