#!/bin/sh
# tests/unit/shell/test_functions.sh — shell function tests for silex
# chmod +x tests/unit/shell/test_functions.sh
# Usage: ./test_functions.sh [path/to/silex]

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

# --- Function definition with name() { } syntax ---
got=$("$MB" -c '
greet() {
    echo "hello from greet"
}
greet')
check "function definition name() { } and call" "$got" "hello from greet"

# --- Function definition with function keyword ---
got=$("$MB" -c '
function greet2 {
    echo "function keyword"
}
greet2')
check "function definition with 'function' keyword" "$got" "function keyword"

# --- Function call with arguments ---
got=$("$MB" -c '
say() {
    echo "arg1=$1 arg2=$2"
}
say hello world')
check "function with arguments \$1 \$2" "$got" "arg1=hello arg2=world"

# --- Function: $# inside function ---
got=$("$MB" -c '
count_args() {
    echo "$#"
}
count_args a b c d')
check "function \$#: argument count" "$got" "4"

# --- Function: $@ inside function ---
got=$("$MB" -c '
list_args() {
    for a in "$@"; do echo "$a"; done
}
list_args x y z')
check 'function "$@": iterate args' "$got" "$(printf 'x\ny\nz')"

# --- Function: $* inside function ---
got=$("$MB" -c '
join_args() {
    echo "$*"
}
join_args a b c')
check 'function "$*": join args' "$got" "a b c"

# --- Local variables ---
got=$("$MB" -c '
OUTER=global
modify() {
    local OUTER=local_val
    echo "inside: $OUTER"
}
modify
echo "outside: $OUTER"')
check "local variable: shadows but does not modify outer" "$got" "$(printf 'inside: local_val\noutside: global')"

# --- Local variable not visible after function returns ---
got=$("$MB" -c '
fn() {
    local HIDDEN=secret
}
fn
echo "${HIDDEN:-not_visible}"')
check "local variable: not visible outside function" "$got" "not_visible"

# --- Return value: explicit return code ---
"$MB" -c '
succeed() { return 0; }
succeed'
check_exit "function return 0: exit code 0" "$?" "0"

"$MB" -c '
fail() { return 1; }
fail'
check_exit "function return 1: exit code 1" "$?" "1"

"$MB" -c '
custom() { return 42; }
custom'
check_exit "function return 42: exit code 42" "$?" "42"

# --- Return value captured in $? ---
got=$("$MB" -c '
get_status() { return 7; }
get_status
echo $?')
check "function return value in \$?" "$got" "7"

# --- Function: implicit return from last command ---
"$MB" -c '
fn() { true; }
fn'
check_exit "implicit return: last cmd true -> 0" "$?" "0"

"$MB" -c '
fn() { false; }
fn'
check_exit "implicit return: last cmd false -> 1" "$?" "1"

# --- Recursive function ---
got=$("$MB" -c '
factorial() {
    local N="$1"
    if [ "$N" -le 1 ]; then
        echo 1
    else
        local PREV
        PREV=$(factorial $((N - 1)))
        echo $((N * PREV))
    fi
}
factorial 5')
check "recursive function: factorial(5) = 120" "$got" "120"

# --- Fibonacci ---
got=$("$MB" -c '
fib() {
    local N="$1"
    if [ "$N" -le 1 ]; then
        echo "$N"
    else
        local A B
        A=$(fib $((N-1)))
        B=$(fib $((N-2)))
        echo $((A + B))
    fi
}
fib 8')
check "recursive function: fib(8) = 21" "$got" "21"

# --- Function can call other functions ---
got=$("$MB" -c '
inner() { echo "inner_result"; }
outer() {
    local R
    R=$(inner)
    echo "outer got: $R"
}
outer')
check "function calls another function" "$got" "outer got: inner_result"

# --- Function redefinition ---
got=$("$MB" -c '
fn() { echo "version1"; }
fn() { echo "version2"; }
fn')
check "function redefinition: last definition wins" "$got" "version2"

# --- Overriding a builtin with a function ---
got=$("$MB" -c '
echo() { printf "OVERRIDE: %s\n" "$*"; }
echo hello')
check "override builtin 'echo' with function" "$got" "OVERRIDE: hello"

# --- Restore overridden builtin via unset -f ---
got=$("$MB" -c '
echo() { printf "OVERRIDE: %s\n" "$*"; }
unset -f echo
echo hello')
check "unset -f restores original builtin" "$got" "hello"

# --- Function with no arguments ---
got=$("$MB" -c '
no_args() {
    echo "no args: $#"
}
no_args')
check "function with no arguments: \$# is 0" "$got" "no args: 0"

# --- Function that modifies global variable ---
got=$("$MB" -c '
COUNTER=0
increment() {
    COUNTER=$((COUNTER + 1))
}
increment
increment
increment
echo $COUNTER')
check "function modifies global variable" "$got" "3"

# --- Nested function definitions ---
got=$("$MB" -c '
outer() {
    inner() { echo "inner called"; }
    inner
}
outer')
check "nested function definition" "$got" "inner called"

# --- Function with heredoc ---
got=$("$MB" -c '
usage() {
    cat <<EOF
Usage: myprog [options]
  -h  help
EOF
}
usage')
check "function with heredoc" "$got" "$(printf 'Usage: myprog [options]\n  -h  help')"

# --- Positional params restored after function ---
got=$("$MB" -c '
set -- outer1 outer2
fn() {
    set -- inner1 inner2 inner3
    echo "in fn: $#"
}
fn
echo "after fn: $#"')
check "positional params restored after function" "$got" "$(printf 'in fn: 3\nafter fn: 2')"

# --- redirections on a function definition apply on every call (POSIX) --------
# `fn() { ...; } >f` remembers the redirect and applies it each time fn runs.
# silex parsed it but dropped the redirect (modernish FTL_FNREDIR).
check "fnredir: definition redirect suppresses output on call" \
    "$("$MB" -c 'fn() { echo hi; } >/dev/null; fn; echo done')" "done"
check "fnredir: FTL_FNREDIR fd setup is honoured" \
    "$("$MB" -c 'fn() { command : <&5; } 5</dev/null; fn 5<&-; echo "rc=$?"')" "rc=0"
check "fnredir: body still executes normally" \
    "$("$MB" -c 'fn() { echo a; echo b; } 2>/dev/null; fn')" "$(printf 'a\nb')"
check "fnredir: plain function without redirect unaffected" \
    "$("$MB" -c 'f() { echo normal; }; f')" "normal"

# --- a plain assignment inside a function is global (POSIX) --------------------
# It was created in the function's scope and lost on return; only `local` should
# be function-local.
check "scope: new variable set in a function is global" \
    "$("$MB" -c 'fn() { NEWV=hi; }; fn; echo "[$NEWV]"')" "[hi]"
check "scope: variable set in a function loop persists" \
    "$("$MB" -c 'fn() { for x in a b c; do LAST=$x; done; }; fn; echo "[$LAST]"')" "[c]"
check "scope: local stays local, plain assignment goes global" \
    "$("$MB" -c 'fn() { local L=loc; G=glob; }; fn; echo "L=[$L] G=[$G]"')" "L=[] G=[glob]"
check "scope: function modifies an existing global" \
    "$("$MB" -c 'V=old; fn() { V=new; }; fn; echo "[$V]"')" "[new]"
check "scope: nested function assignment reaches the top" \
    "$("$MB" -c 'o() { i() { DEEP=x; }; i; }; o; echo "[$DEEP]"')" "[x]"

# --- `command NAME` bypasses aliases and functions of NAME -------------------
# POSIX: command runs the builtin/external, not a function or alias of the same
# name. `alias exit=fn; fn() { command exit; }` (modernish's pattern) must reach
# the real exit, not recurse -- silex used to loop into a stack-overflow crash.
check "command bypasses an alias" \
    "$("$MB" -c 'alias echo="echo ALIASED"; command echo hi')" "hi"
check "command bypasses a function" \
    "$("$MB" -c 'echo() { printf FUNC; }; command echo real')" "real"
check "command exit through an exit-alias does not recurse" \
    "$("$MB" -c 'alias exit=myf; myf() { echo once; command exit 3; }; exit'; echo "rc=$?")" "$(printf 'once\nrc=3')"
# ...but the flag must NOT leak into code that command runs:
check "command eval still resolves functions in its code" \
    "$("$MB" -c 'f() { echo infunc; }; command eval f')" "infunc"
check "command eval runs multiple statements with normal lookup" \
    "$("$MB" -c 'g() { printf G; }; command eval "g; echo X"')" "$(printf 'GX')"

echo ""
echo "function tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
