#!/bin/sh
# tests/unit/shell/test_errexit.sh — set -e (errexit) edge case tests for silex
# Usage: ./test_errexit.sh [path/to/silex]

MB="${1:-build/bin/silex}"
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

# --- set -e: shell exits on false ---
"$MB" -c 'set -e; false; echo should_not_print' 2>/dev/null
check_exit "set -e: false causes exit" "$?" "1"

# --- set -e: false in if condition does NOT exit ---
got=$("$MB" -c 'set -e; if false; then echo yes; else echo no; fi')
check "set -e: false in if condition exempt" "$got" "no"

# --- set -e: false || true does NOT exit ---
got=$("$MB" -c 'set -e; false || true; echo continued')
check "set -e: false || true does not exit" "$got" "continued"

# --- set -e: ! false does NOT exit ---
got=$("$MB" -c 'set -e; ! false; echo continued')
check "set -e: ! false does not exit" "$got" "continued"

# --- set -e: false && true (AND left side) does NOT exit ---
got=$("$MB" -c 'set -e; false && echo yes; echo continued')
check "set -e: false && ... does not exit" "$got" "continued"

# --- set -e: function returning 1 exits ---
"$MB" -c 'set -e; f() { return 1; }; f; echo should_not_print' 2>/dev/null
check_exit "set -e: function return 1 causes exit" "$?" "1"

# --- set -e: false in while condition does NOT exit ---
got=$("$MB" -c 'set -e; while false; do echo loop; done; echo continued')
check "set -e: false in while condition exempt" "$got" "continued"

# --- set -e: false in until condition does NOT exit ---
got=$("$MB" -c 'set -e; until true; do echo loop; done; echo continued')
check "set -e: until true condition exempt" "$got" "continued"

# --- set -e: subshell false exits parent ---
"$MB" -c 'set -e; (false); echo should_not_print' 2>/dev/null
check_exit "set -e: subshell false exits parent" "$?" "1"

# --- without set -e: false does NOT exit ---
got=$("$MB" -c 'false; echo continued')
check "without set -e: false does not exit" "$got" "continued"

echo
echo "errexit tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
