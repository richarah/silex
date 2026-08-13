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

# --- set -e: a `!` pipeline is exempt, INCLUDING its own inverted result ---
# POSIX 2.9.2: -e is ignored when the pipeline begins with `!`. Running the
# negated command in a condition context is not enough -- `! true` yields 1,
# and that 1 must not kill the shell either.
got=$("$MB" -c 'set -o errexit; echo one; ! true; echo two; ! false; echo three')
check "set -e: '! true' does not exit" "$got" "one
two
three"

got=$("$MB" -c 'set -o errexit; echo one; ! true; echo two')
check "set -e: '! true' on one line does not exit" "$got" "one
two"

# The exemption reaches through a function call: `! f` where f runs `set -e;
# false` must neither stop f nor stop the caller.
got=$("$MB" -c 'f() { set -e; false; echo in_f; }; set -e; ! f; echo after')
check "set -e: '! f' exempts the function body and the result" "$got" "in_f
after"

echo
echo "errexit tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
