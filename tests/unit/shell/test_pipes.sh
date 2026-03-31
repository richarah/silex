#!/bin/sh
# tests/unit/shell/test_pipes.sh — pipe functionality tests for silex shell
# chmod +x tests/unit/shell/test_pipes.sh
# Usage: ./test_pipes.sh [path/to/silex]

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
        echo "  expected: $(printf '%s' "$expected" | cat -A)"
        echo "  got:      $(printf '%s' "$got" | cat -A)"
        FAIL=$((FAIL + 1))
    fi
}

check_exit() {
    local desc="$1"
    local got_exit="$2"
    local expected_exit="$3"
    if [ "$got_exit" = "$expected_exit" ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected exit: $expected_exit"
        echo "  got exit:      $got_exit"
        FAIL=$((FAIL + 1))
    fi
}

MB="$SILEX"

# --- Simple pipe: echo hello | cat ---
got=$("$MB" -c 'echo hello | cat')
check "simple pipe: echo hello | cat" "$got" "hello"

# --- Multi-stage pipe: echo hello | tr a-z A-Z | cat ---
got=$("$MB" -c 'echo hello | tr a-z A-Z | cat')
check "multi-stage pipe: echo -> tr -> cat" "$got" "HELLO"

# --- Pipe with grep ---
got=$("$MB" -c 'printf "a\nb\nc\n" | grep b')
check "pipe with grep: select single line" "$got" "b"

# --- Multi-line grep through pipe ---
got=$("$MB" -c 'printf "foo\nbar\nbaz\n" | grep ba')
check "pipe with grep: match multiple lines" "$got" "$(printf 'bar\nbaz')"

# --- Pipe exit code: last command determines pipeline exit ---
"$MB" -c 'echo hello | true'
check_exit "pipe exit: last stage true -> 0" "$?" "0"

"$MB" -c 'echo hello | false'
check_exit "pipe exit: last stage false -> 1" "$?" "1"

# --- Pipe exit code: grep no match -> 1 ---
"$MB" -c 'echo hello | grep nomatch' > /dev/null 2>&1
check_exit "pipe exit: grep no match -> 1" "$?" "1"

# --- Pipe with output redirect ---
TMPDIR_PIPES=$(mktemp -d)
trap 'rm -rf "$TMPDIR_PIPES"' EXIT INT TERM
"$MB" -c "echo pipeout | cat > $TMPDIR_PIPES/out.txt"
got=$(cat "$TMPDIR_PIPES/out.txt")
check "pipe with redirect: output written to file" "$got" "pipeout"

# --- Pipe with input from file ---
printf 'line1\nline2\nline3\n' > "$TMPDIR_PIPES/input.txt"
got=$("$MB" -c "cat $TMPDIR_PIPES/input.txt | grep line2")
check "pipe with input from file" "$got" "line2"

# --- Three-stage pipe preserving data ---
got=$("$MB" -c 'printf "banana\napple\ncherry\n" | sort | head -1')
check "three-stage pipe: sort | head -1" "$got" "apple"

# --- Pipe with wc ---
got=$("$MB" -c 'printf "one\ntwo\nthree\n" | wc -l')
# wc -l output may have leading whitespace; trim it
got=$(printf '%s' "$got" | tr -d ' ')
check "pipe with wc -l: count lines" "$got" "3"

# --- Empty-ish pipe: cat /dev/null | cat ---
got=$("$MB" -c 'cat /dev/null | cat')
check "pipe with empty input: cat /dev/null | cat" "$got" ""

# --- Pipe with echo -n (no trailing newline) ---
got=$("$MB" -c 'echo -n "noeol" | cat')
check "pipe with echo -n: no trailing newline passed through" "$got" "noeol"

# --- Nested pipes in subshell ---
got=$("$MB" -c '(echo nested | tr n N)')
check "pipe in subshell" "$got" "Nested"

# --- Pipe: write then count with wc -c ---
got=$("$MB" -c 'echo -n "abc" | wc -c')
got=$(printf '%s' "$got" | tr -d ' ')
check "pipe with wc -c: byte count" "$got" "3"

# --- Pipe stderr not captured by next stage ---
got=$("$MB" -c 'echo stdout | cat' 2>/dev/null)
check "pipe: only stdout flows through pipe (not stderr)" "$got" "stdout"

# --- Long pipeline: 4 stages ---
got=$("$MB" -c 'echo "mixedCASE" | tr A-Z a-z | tr a-z A-Z | cat')
check "long pipeline: 4 stages" "$got" "MIXEDCASE"

echo ""
echo "pipe tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
