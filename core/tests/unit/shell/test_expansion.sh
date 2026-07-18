#!/bin/sh
# tests/unit/shell/test_expansion.sh — parameter expansion tests for silex
# chmod +x tests/unit/shell/test_expansion.sh
# Usage: ./test_expansion.sh [path/to/silex]

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

# ===========================================================================
# Heredoc: variable expansion and no-expand (B-02)
# ===========================================================================

# Unquoted <<EOF: variables ARE expanded
got=$("$MB" -c 'x=hello; cat <<EOF
$x world
EOF')
check "heredoc <<EOF: variable expanded" "$got" "hello world"

# Single-quoted <<'EOF': variables are NOT expanded
got=$("$MB" -c "x=hello; cat <<'EOF'
\$x is not expanded
EOF")
check "heredoc <<'EOF': variable not expanded" "$got" '$x is not expanded'

# Double-quoted <<"EOF": variables are NOT expanded
got=$("$MB" -c 'x=hello; cat <<"EOF"
$x is not expanded
EOF')
check 'heredoc <<"EOF": variable not expanded' "$got" '$x is not expanded'

# Backslash-quoted delimiter: variables are NOT expanded
got=$("$MB" -c 'x=hello; cat <<\EOF
$x is not expanded
EOF')
check 'heredoc <<\EOF: variable not expanded' "$got" '$x is not expanded'

# Heredoc with multiple variables (unquoted)
got=$("$MB" -c 'a=foo; b=bar; cat <<END
$a $b
END')
check "heredoc <<END: multiple variables expanded" "$got" "foo bar"

# --- literal 0x01 bytes must survive expansion (not collide with the "$@" mark) -
# The field splitter uses 0x01 internally to mark "$@" boundaries. A literal 0x01
# in real data (command substitution, a variable) was indistinguishable and got
# dropped -- breaking modernish's FTL_ROASSIGN/FTL_CASECC, which test ^A. The
# split now runs only when "$@" actually produced a boundary.
check "0x01: survives a command-substitution round-trip" \
    "$("$MB" -c 'A=$(printf "x\001y"); printf "%s" "$A" | od -An -tx1' 2>&1 | tr -s ' ')" \
    " 78 01 79"
check "0x01: stored variable length is 1" \
    "$("$MB" -c 'Z=$(printf "\001"); echo ${#Z}')" "1"
check "0x01: a var with 0x01 matches an identical var pattern in case" \
    "$("$MB" -c 'A=$(printf "\001"); B=$A; case "$A" in "$B") echo M;; *) echo N;; esac')" "M"
check "0x01: 0x02 also survives (control bytes generally)" \
    "$("$MB" -c 'A=$(printf "a\002b"); printf "%s" "$A" | od -An -tx1' 2>&1 | tr -s ' ')" \
    " 61 02 62"

# The 0x01 fix must NOT break "$@" field splitting.
check "\$@: still splits into separate fields" \
    "$("$MB" -c 'set -- a b c; for x in "$@"; do printf "[%s]" "$x"; done')" "[a][b][c]"
check "\$@: preserves embedded spaces as field boundaries" \
    "$("$MB" -c 'set -- "a b" "c d"; printf "[%s]" "$@"')" "[a b][c d]"
check "\$@: a 0x01 inside a positional still yields the field" \
    "$("$MB" -c 'p=$(printf "p\001q"); set -- "$p"; printf "%s" "$1" | od -An -tx1' 2>&1 | tr -s ' ')" \
    " 70 01 71"

# --- ${v#pat} / ${v%pat} expand and quote-process the pattern (POSIX) ---------
# The pattern used the raw text, so a nested expansion or quoting inside it was
# ignored -- modernish FTL_PSUB2. It is now expanded and quote-aware, like a
# case pattern.
check "psub: %pat strips the literal a nested expansion yields" \
    "$("$MB" -c 't=$(printf "a\${x=B}"); unset -v x; printf "%s" "${t%"${t#a}"}"')" "a"
check "psub: unquoted pattern var is an active glob" \
    "$("$MB" -c 'p="b*"; v=abcxyz; printf "%s" "${v#$p}"')" "abcxyz"
check "psub: quoted pattern var is literal" \
    "$("$MB" -c 'p="b*"; v="ab*z"; printf "%s" "${v#a"$p"}"')" "z"
# Regression: ordinary #/% still work.
check "psub: basename via ##*/" \
    "$("$MB" -c 't=/a/b/c.txt; printf "%s" "${t##*/}"')" "c.txt"
check "psub: strip extension via %.ext" \
    "$("$MB" -c 'v=file.tar.gz; printf "%s" "${v%.gz}"')" "file.tar"
check "psub: %%* yields empty" \
    "$("$MB" -c 't=hello; printf "[%s]" "${t%%*}"')" "[]"

echo ""
echo "expansion tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
