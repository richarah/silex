#!/bin/sh
# tests/unit/shell/test_variables.sh — shell variable tests for matchbox
# chmod +x tests/unit/shell/test_variables.sh
# Usage: ./test_variables.sh [path/to/matchbox]

MATCHBOX="${1:-build/bin/matchbox}"
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

MB="$MATCHBOX"

# --- Simple assignment and expansion ---
got=$("$MB" -c 'X=hello; echo $X')
check "simple assignment: echo \$X" "$got" "hello"

got=$("$MB" -c 'A=foo; B=bar; echo $A$B')
check "concatenated expansion: \$A\$B" "$got" "foobar"

# --- Variable with spaces (quoted) ---
got=$("$MB" -c 'MSG="hello world"; echo "$MSG"')
check "variable with spaces, quoted expansion" "$got" "hello world"

# --- Unquoted variable with spaces: word splitting ---
got=$("$MB" -c 'V="a b c"; echo $V')
check "unquoted: word split produces same echo output" "$got" "a b c"

# --- export: visible in child process ---
got=$("$MB" -c 'export MY_VAR=exported; sh -c "echo \$MY_VAR"')
check "export: variable visible in child sh" "$got" "exported"

# --- Non-exported variable: not visible in child ---
got=$("$MB" -c 'NOT_EXPORTED=hidden; sh -c "echo ${NOT_EXPORTED:-absent}"')
check "unexported variable not visible in child" "$got" "absent"

# --- readonly: assignment rejected ---
"$MB" -c 'readonly RO=fixed; RO=changed' 2>/dev/null
check_exit "readonly: reassignment fails (non-zero exit)" "$?" "1"

got=$("$MB" -c 'readonly RO=fixed; echo $RO')
check "readonly: value still readable" "$got" "fixed"

# --- readonly: unset rejected ---
"$MB" -c 'readonly RO2=x; unset RO2' 2>/dev/null
check_exit "readonly: unset fails (non-zero exit)" "$?" "1"

# --- local in functions ---
got=$("$MB" -c '
f() {
    local LOCAL_VAR=inside
    echo $LOCAL_VAR
}
f
echo "${LOCAL_VAR:-outside}"
')
check "local: function local var not visible outside" "$got" "$(printf 'inside\noutside')"

# --- unset variable ---
got=$("$MB" -c 'X=set; unset X; echo "${X:-unset}"')
check "unset: variable gone after unset" "$got" "unset"

# --- unset function ---
got=$("$MB" -c '
fn() { echo fn_result; }
unset -f fn
fn 2>/dev/null || echo fn_gone
')
check "unset -f: function removed" "$got" "fn_gone"

# --- $? exit status of last command ---
got=$("$MB" -c 'true; echo $?')
check "\$? after true: 0" "$got" "0"

got=$("$MB" -c 'false; echo $?')
check "\$? after false: 1" "$got" "1"

got=$("$MB" -c 'sh -c "exit 42"; echo $?')
check "\$? reflects child exit code 42" "$got" "42"

# --- $# argument count ---
got=$("$MB" -c 'set -- a b c; echo $#')
check "\$# after set -- a b c: 3" "$got" "3"

got=$("$MB" -c 'set --; echo $#')
check "\$# after set --: 0" "$got" "0"

# --- $@ all positional parameters ---
got=$("$MB" -c 'set -- x y z; echo "$@"')
check "\$@ expands all positional params" "$got" "x y z"

# --- "$@" preserves word boundaries ---
got=$("$MB" -c 'set -- "a b" c; for a in "$@"; do echo "$a"; done')
check '"\$@" preserves word boundaries' "$got" "$(printf 'a b\nc')"

# --- $* all positional params as single string ---
got=$("$MB" -c 'set -- p q r; echo $*')
check "\$* expands all positional params" "$got" "p q r"

# --- IFS splitting ---
got=$("$MB" -c 'IFS=:; V="a:b:c"; for x in $V; do echo $x; done')
check "IFS=: splits on colon" "$got" "$(printf 'a\nb\nc')"

# --- IFS= prevents splitting ---
got=$("$MB" -c 'IFS=; V="a b c"; for x in $V; do echo ">$x<"; done')
check "IFS= prevents word splitting" "$got" ">a b c<"

# --- $$ PID is a non-empty integer ---
got=$("$MB" -c 'echo $$')
if printf '%s' "$got" | grep -qE '^[0-9]+$'; then
    echo "PASS: \$\$ is a numeric PID"
    PASS=$((PASS + 1))
else
    echo "FAIL: \$\$ is not numeric: '$got'"
    FAIL=$((FAIL + 1))
fi

# --- $! PID of last backgrounded process ---
got=$("$MB" -c 'sleep 0 &; BGPID=$!; wait; echo $BGPID')
if printf '%s' "$got" | grep -qE '^[0-9]+$'; then
    echo "PASS: \$! is a numeric PID of bg process"
    PASS=$((PASS + 1))
else
    echo "FAIL: \$! is not numeric: '$got'"
    FAIL=$((FAIL + 1))
fi

# --- Positional parameters $1 through $9 ---
got=$("$MB" -c 'set -- a b c d e f g h i; echo $1 $5 $9')
check "\$1 \$5 \$9 positional parameters" "$got" "a e i"

# --- Positional parameter ${10} and beyond ---
got=$("$MB" -c 'set -- a b c d e f g h i j k; echo ${10} ${11}')
check "\${10} \${11} beyond single digit" "$got" "j k"

# --- Assign and use in same command: env var for child ---
got=$("$MB" -c 'VAR=val sh -c "echo \$VAR"')
check "env assignment prefix: VAR=val cmd" "$got" "val"

# --- Nested variable names ---
got=$("$MB" -c 'OUTER=hello_outer; echo $OUTER')
check "variable name with underscore" "$got" "hello_outer"

# --- Empty variable expansion ---
got=$("$MB" -c 'EMPTY=; echo "${EMPTY}suffix"')
check "empty variable: \${EMPTY}suffix" "$got" "suffix"

# --- Variable in arithmetic ---
got=$("$MB" -c 'N=10; echo $((N + 5))')
check "variable in arithmetic \$((N + 5))" "$got" "15"

echo ""
echo "variable tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
