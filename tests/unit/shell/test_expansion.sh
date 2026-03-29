#!/bin/sh
# tests/unit/shell/test_expansion.sh — parameter expansion tests for matchbox
# chmod +x tests/unit/shell/test_expansion.sh
# Usage: ./test_expansion.sh [path/to/matchbox]

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

# --- ${var:-default}: use default if unset or empty ---
got=$("$MB" -c 'unset V; echo ${V:-default}')
check "\${var:-default}: unset var -> default" "$got" "default"

got=$("$MB" -c 'V=; echo ${V:-default}')
check "\${var:-default}: empty var -> default" "$got" "default"

got=$("$MB" -c 'V=set; echo ${V:-default}')
check "\${var:-default}: set var -> var value" "$got" "set"

# --- ${var:=assign}: assign and use default if unset or empty ---
got=$("$MB" -c 'unset V; echo ${V:=assigned}; echo $V')
check "\${var:=assign}: unset var -> assigns and expands" "$got" "$(printf 'assigned\nassigned')"

got=$("$MB" -c 'V=existing; echo ${V:=assigned}')
check "\${var:=assign}: set var -> not reassigned" "$got" "existing"

# --- ${var:+alternate}: use alternate if set and non-empty ---
got=$("$MB" -c 'V=set; echo ${V:+alternate}')
check "\${var:+alternate}: set var -> alternate" "$got" "alternate"

got=$("$MB" -c 'unset V; echo ${V:+alternate}')
check "\${var:+alternate}: unset var -> empty string" "$got" ""

got=$("$MB" -c 'V=; echo ${V:+alternate}')
check "\${var:+alternate}: empty var -> empty string" "$got" ""

# --- ${var:?error}: exit with error if unset or empty ---
"$MB" -c 'unset V; echo ${V:?must be set}' 2>/dev/null
check_exit "\${var:?error}: unset var -> non-zero exit" "$?" "1"

got=$("$MB" -c 'V=ok; echo ${V:?must be set}')
check "\${var:?error}: set var -> value returned" "$got" "ok"

# --- ${#var}: string length ---
got=$("$MB" -c 'V=hello; echo ${#V}')
check "\${#var}: length of 'hello'" "$got" "5"

got=$("$MB" -c 'V=; echo ${#V}')
check "\${#var}: length of empty string" "$got" "0"

got=$("$MB" -c 'V=abcdefghij; echo ${#V}')
check "\${#var}: length of 10-char string" "$got" "10"

# --- ${var#prefix}: remove shortest prefix match ---
got=$("$MB" -c 'V=hello_world; echo ${V#hello_}')
check "\${var#prefix}: remove shortest prefix" "$got" "world"

got=$("$MB" -c 'V=/usr/local/bin; echo ${V#/*/}')
check "\${var#/*/}: remove shortest wildcard prefix" "$got" "local/bin"

# --- ${var##prefix}: remove longest prefix match ---
got=$("$MB" -c 'V=/usr/local/bin; echo ${V##*/}')
check "\${var##*/}: remove longest prefix (basename)" "$got" "bin"

got=$("$MB" -c 'V=aXbXc; echo ${V##*X}')
check "\${var##*X}: remove longest prefix up to last X" "$got" "c"

# --- ${var%suffix}: remove shortest suffix match ---
got=$("$MB" -c 'V=file.tar.gz; echo ${V%.*}')
check "\${var%.*}: remove shortest suffix (last ext)" "$got" "file.tar"

got=$("$MB" -c 'V=/usr/local/bin; echo ${V%/*}')
check "\${var%/*}: remove shortest suffix (dirname)" "$got" "/usr/local"

# --- ${var%%suffix}: remove longest suffix match ---
got=$("$MB" -c 'V=file.tar.gz; echo ${V%%.*}')
check "\${var%%.*}: remove longest suffix (all exts)" "$got" "file"

got=$("$MB" -c 'V=aXbXc; echo ${V%%X*}')
check "\${var%%X*}: remove longest suffix from first X" "$got" "a"

# --- ${var/pat/repl}: replace first occurrence ---
got=$("$MB" -c 'V=hello_world_hello; echo ${V/hello/bye}')
check "\${var/pat/repl}: replace first occurrence" "$got" "bye_world_hello"

got=$("$MB" -c 'V=aababab; echo ${V/ab/XY}')
check "\${var/ab/XY}: replace first ab" "$got" "aXYabab"

# --- ${var//pat/repl}: replace all occurrences ---
got=$("$MB" -c 'V=hello_world_hello; echo ${V//hello/bye}')
check "\${var//pat/repl}: replace all occurrences" "$got" "bye_world_bye"

got=$("$MB" -c 'V=aababab; echo ${V//ab/XY}')
check "\${var//ab/XY}: replace all ab" "$got" "aXYXYXY"

# --- ${var/pat/}: delete first occurrence (empty replacement) ---
got=$("$MB" -c 'V=hello_world; echo ${V/hello_/}')
check "\${var/pat/}: delete first match" "$got" "world"

# --- Tilde expansion: ~/ ---
HOME_DIR="$HOME"
got=$("$MB" -c 'echo ~/foo')
check "tilde expansion ~/foo -> \$HOME/foo" "$got" "$HOME_DIR/foo"

got=$("$MB" -c 'echo ~')
check "bare tilde -> \$HOME" "$got" "$HOME_DIR"

# --- Command substitution $(cmd) ---
got=$("$MB" -c 'echo $(echo inner)')
check "command substitution \$(cmd)" "$got" "inner"

got=$("$MB" -c 'D=$(date +%Y); echo $D' )
if printf '%s' "$got" | grep -qE '^[0-9]{4}$'; then
    echo "PASS: \$(date +%Y) gives 4-digit year"
    PASS=$((PASS + 1))
else
    echo "FAIL: \$(date +%Y) unexpected: '$got'"
    FAIL=$((FAIL + 1))
fi

# --- Nested command substitution ---
got=$("$MB" -c 'echo $(echo $(echo deep))')
check "nested command substitution" "$got" "deep"

# --- Backtick command substitution ---
got=$("$MB" -c 'echo `echo backtick`')
check "backtick command substitution" "$got" "backtick"

# --- Arithmetic $((expr)) ---
got=$("$MB" -c 'echo $((2 + 3))')
check "arithmetic \$((2 + 3))" "$got" "5"

got=$("$MB" -c 'echo $((10 - 4))')
check "arithmetic \$((10 - 4))" "$got" "6"

got=$("$MB" -c 'echo $((3 * 7))')
check "arithmetic \$((3 * 7))" "$got" "21"

got=$("$MB" -c 'echo $((17 / 4))')
check "arithmetic \$((17 / 4)) integer division" "$got" "4"

got=$("$MB" -c 'echo $((17 % 4))')
check "arithmetic \$((17 % 4)) modulo" "$got" "1"

got=$("$MB" -c 'echo $((1 << 3))')
check "arithmetic \$((1 << 3)) bitshift" "$got" "8"

got=$("$MB" -c 'A=5; B=3; echo $((A * B))')
check "arithmetic with variable operands" "$got" "15"

# --- Arithmetic comparison (returns 0 or 1) ---
got=$("$MB" -c 'echo $((5 > 3))')
check "arithmetic comparison: 5 > 3 -> 1" "$got" "1"

got=$("$MB" -c 'echo $((2 > 9))')
check "arithmetic comparison: 2 > 9 -> 0" "$got" "0"

# --- Arithmetic assignment ---
got=$("$MB" -c 'X=0; echo $((X += 5)); echo $X')
check "arithmetic assignment \$((X += 5)) modifies X" "$got" "$(printf '5\n5')"

echo ""
echo "expansion tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
