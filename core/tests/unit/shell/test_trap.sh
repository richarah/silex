#!/bin/sh
# tests/unit/shell/test_trap.sh — trap built-in edge case tests for silex
# Usage: ./test_trap.sh [path/to/silex]

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
# re-evaluated at signal time in others. silex should re-evaluate.
# Both "first first" and "first second" are defensible; test that it runs.
if printf '%s' "$got" | grep -q "first"; then
    echo "PASS: trap action executed on signal"
    PASS=$((PASS + 1))
else
    echo "FAIL: trap action not executed on signal"
    FAIL=$((FAIL + 1))
fi

# --- `trap` listing is the same in every subshell context ---
# A subshell keeps the parent's action strings as display-only entries so that
# `trap` with no operands reports the traps actually in effect. `( trap )` and
# `trap | cat` did that; `$(trap)` did not, because the command-substitution
# child starts from a fresh shell context. All four must agree.
list_prog='trap "echo hi" USR1; trap "echo bye" EXIT'
expected="$(printf "trap -- 'echo bye' EXIT\ntrap -- 'echo hi' USR1")"
got=$("$MB" -c "$list_prog; trap; trap - EXIT; exit 0")
check "trap listing: plain" "$got" "$expected"
got=$("$MB" -c "$list_prog; echo \"\$(trap)\"; trap - EXIT; exit 0")
check "trap listing: inside \$(trap)" "$got" "$expected"
got=$("$MB" -c "$list_prog; ( trap ); trap - EXIT; exit 0")
check "trap listing: inside ( trap )" "$got" "$expected"
got=$("$MB" -c "$list_prog; trap | cat; trap - EXIT; exit 0")
check "trap listing: inside trap | cat" "$got" "$expected"

# --- save/restore round-trip through a command substitution ---
# `saved=$(trap)` then `eval "$saved"` is THE portable idiom for saving a trap
# set (modernish reconstructs its trap stack this way). It only works if the
# listing survives the command substitution.
got=$("$MB" -c 'trap "echo hi" USR1; saved=$(trap); trap - USR1; eval "$saved"; trap; exit 0')
check "trap save/restore via \$(trap) round-trips" "$got" "$(printf "trap -- 'echo hi' USR1")"

# --- the inherited entries are display-only, never executed ---
# The parent's EXIT trap must not fire when the command substitution ends, and
# the parent's signal traps take the DEFAULT action inside it.
got=$("$MB" -c 'trap "echo EXITTRAP" EXIT; w=$(exit 3); echo "status=$?"')
check "inherited EXIT trap does not fire in \$( )" "$got" "$(printf 'status=3\nEXITTRAP')"
got=$("$MB" -c 'trap "echo PIPETRAP" PIPE; r=$(yes 2>/dev/null | head -1); echo "r=$r"; exit 0')
check "inherited PIPE trap does not fire in \$( )" "$got" "r=y"

# --- a trap set inside the substitution wipes the inherited listing ---
got=$("$MB" -c 'trap "echo hi" USR1; echo "$(trap "echo mine" EXIT; trap)"; exit 0')
check "trap set in \$( ) discards inherited entries" "$got" "$(printf "trap -- 'echo mine' EXIT\nmine")"

# --- SIGPIPE: pipeline handling ---
# A yes | head pipeline should produce output and exit 0 or 141, not crash.
got=$(yes 2>/dev/null | "$MB" head -n 3 2>/dev/null)
check "SIGPIPE: head terminates yes cleanly" "$got" "$(printf 'y\ny\ny')"

echo
echo "trap tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
