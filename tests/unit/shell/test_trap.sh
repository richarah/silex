#!/bin/sh
# tests/unit/shell/test_trap.sh — trap built-in edge case tests for matchbox
# Usage: ./test_trap.sh [path/to/matchbox]

MB="${1:-build/bin/matchbox}"
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

# --- trap '' SIGINT: ignore signal ---
got=$("$MB" -c 'trap "" INT; kill -INT $$; echo survived')
check "trap '' INT: shell survives SIGINT" "$got" "survived"

# --- trap - SIGINT: reset to default ---
# After reset, SIGINT should terminate the shell; hard to test portably,
# so just verify the command succeeds without error.
"$MB" -c 'trap "" INT; trap - INT' 2>/dev/null
check_exit "trap - INT: reset to default succeeds" "$?" "0"

# --- trap 'CMD' EXIT: run on exit ---
got=$("$MB" -c 'trap "echo cleanup" EXIT; echo before')
check "trap EXIT: runs on exit" "$got" "$(printf 'before\ncleanup')"

# --- trap 'CMD' EXIT: runs even on normal exit ---
got=$("$MB" -c 'trap "echo done" EXIT; exit 0')
check "trap EXIT: runs on exit 0" "$got" "done"

# --- trap 'CMD' EXIT: runs with nonzero exit ---
"$MB" -c 'trap "echo trapped" EXIT; exit 1' 2>/dev/null
check_exit "trap EXIT with exit 1: exit code preserved" "$?" "1"

# --- trap with multiple signals ---
got=$("$MB" -c 'trap "echo caught" USR1 USR2; kill -USR1 $$; kill -USR2 $$; echo done')
check "trap multiple signals: both caught" "$got" "$(printf 'caught\ncaught\ndone')"

# --- trap action is re-evaluated on each signal ---
got=$("$MB" -c 'X=first; trap "echo $X" USR1; kill -USR1 $$; X=second; kill -USR1 $$')
# Note: trap action string is captured at trap time in some shells,
# re-evaluated at signal time in others. matchbox should re-evaluate.
# Both "first first" and "first second" are defensible; test that it runs.
if printf '%s' "$got" | grep -q "first"; then
    echo "PASS: trap action executed on signal"
    PASS=$((PASS + 1))
else
    echo "FAIL: trap action not executed on signal"
    FAIL=$((FAIL + 1))
fi

# --- SIGPIPE: pipeline handling ---
# A yes | head pipeline should produce output and exit 0 or 141, not crash.
got=$(yes 2>/dev/null | "$MB" head -n 3 2>/dev/null)
check "SIGPIPE: head terminates yes cleanly" "$got" "$(printf 'y\ny\ny')"

echo
echo "trap tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
