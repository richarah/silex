#!/bin/sh
# tests/unit/shell/test_syntax_errors.sh
#
# Regression tests for input the shell used to ACCEPT. Every case here ran
# silently (usually with status 0) before, which is worse than a wrong answer:
# a typo produced no diagnostic and the script carried on.
#
#   1. Unterminated quotes and substitutions. `echo 'abc` printed abc and
#      exited 0 -- the runaway quote swallowed the rest of the input. Same for
#      ", `, $(, ${ and $((.  Now a syntax error, status 2, like dash.
#   2. Leftover scope terminators. `echo 1 ;; echo 2` printed 1 and THEN
#      reported the error, because the stray `;;` was only met on the next
#      parse call -- after `echo 1` had already run. dash reports it first and
#      runs nothing. Same for done/fi/esac/then/else/elif/do/}/).
#   3. A separator with no command before it: `;`, `; echo b`, `echo a; ; b`
#      and `cmd &;` were all no-ops. The grammar has no production for them.
#   4. A syntax error INSIDE a command substitution is a syntax error of the
#      script: `echo $(if true)` substituted the empty string and exited 0.
#   5. An unknown parameter-expansion operator (`${a&}`) returned the plain
#      value, as if the operator had not been written.
#   6. `export -p`, `alias` and `trap` listings must be re-inputtable: the
#      value went between bare quotes, so any value CONTAINING a quote came
#      back mangled and `eval "$(export -p)"` could not round-trip it.
#
# Usage: ./test_syntax_errors.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
case "$SILEX" in /*) ;; *) SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;; esac
MB="$SILEX"
PASS=0
FAIL=0

check() {
    desc="$1"; got="$2"; expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"; PASS=$((PASS+1))
    else
        echo "FAIL: $desc"
        echo "  expected: [$expected]"
        echo "  got:      [$got]"
        FAIL=$((FAIL+1))
    fi
}

# Run CODE and report "status|stdout". stderr is dropped: the diagnostic's
# wording is not the contract, but the status and the ABSENCE of output are.
run() {
    out=$(printf '%s\n' "$1" | "$MB" 2>/dev/null)
    printf '%s|%s' "$?" "$out"
}

# -----------------------------------------------------------------------
# 1. Unterminated quotes and substitutions
# -----------------------------------------------------------------------
check "unterminated single quote is an error"   "$(run "echo 'abc")"     "2|"
check "unterminated double quote is an error"   "$(run 'echo "abc')"     "2|"
check "unterminated backtick is an error"       "$(run 'echo `abc')"     "2|"
check "unterminated \$( is an error"            "$(run 'echo $(abc')"    "2|"
check "unterminated \${ is an error"            "$(run 'echo ${abc')"    "2|"
check "unterminated \$(( is an error"           "$(run 'echo $((1+2')"   "2|"
# The point of the fix: the runaway construct used to eat the FOLLOWING lines
# and run them as its own contents.
check "unterminated \$( does not swallow the next line" \
    "$(run 'echo $(echo inner
echo after')" "2|"
check "unterminated quote does not swallow the next line" \
    "$(run "echo 'inner
echo after")" "2|"

# ...while the terminated forms are of course still fine.
check "terminated quotes/subs still work" \
    "$("$MB" -c 'echo "a" '"'"'b'"'"' $(echo c) `echo d` ${HOME:+e} $((1+2))')" \
    "a b c d e 3"
check "a quoted string may still span lines" \
    "$("$MB" -c 'echo "one
two"')" "one
two"

# -----------------------------------------------------------------------
# 2. Leftover scope terminators run nothing first
# -----------------------------------------------------------------------
check "stray ;; reports before running the command"   "$(run 'echo 1 ;; echo 2')" "2|"
check "stray done reports before running"             "$(run 'echo 1; done')"     "2|"
check "stray fi reports before running"               "$(run 'echo 1; fi')"       "2|"
check "stray esac reports before running"             "$(run 'echo 1; esac')"     "2|"
check "stray then reports before running"             "$(run 'echo 1; then')"     "2|"
check "stray else reports before running"             "$(run 'echo 1; else')"     "2|"
check "stray do reports before running"               "$(run 'echo 1; do')"       "2|"
check "stray } reports before running"                "$(run 'echo 1; }')"        "2|"
check "stray ) reports before running"                "$(run 'echo 1; )')"        "2|"

# The same tokens must still close their own constructs.
check "case/esac still parses"      "$("$MB" -c 'case a in a) echo hit ;; b) echo no ;; esac')" "hit"
check "if/fi still parses"          "$("$MB" -c 'if true; then echo t; fi')"                    "t"
check "for/done still parses"       "$("$MB" -c 'for i in 1 2; do echo $i; done')"              "1
2"
check "brace group still parses"    "$("$MB" -c '{ echo b; }')"                                 "b"
check "subshell still parses"       "$("$MB" -c '(echo s)')"                                    "s"

# -----------------------------------------------------------------------
# 3. A separator with no command before it
# -----------------------------------------------------------------------
check "bare ; is an error"              "$(run ';')"                  "2|"
check "leading ; is an error"           "$(run '; echo b')"           "2|"
check "doubled ; is an error"           "$(run 'echo a; ; echo b')"   "2|"
check "; on its own line is an error"   "$(run 'echo a
; echo b')" "2|a"
# `&` already terminates a command, so `&;` has nothing before the `;`.
check "&; is an error"                  "$(run 'echo a &; echo b')"   "2|"

check "trailing ; is still fine"        "$(run 'echo a;')"            "0|a"
check "; between commands still fine"   "$(run 'echo a; echo b')"     "0|a
b"
check "& between commands still fine"   "$("$MB" -c 'sleep 0 & wait; echo done')" "done"

# -----------------------------------------------------------------------
# 4. A syntax error inside a command substitution kills the script
# -----------------------------------------------------------------------
check "parse error in \$() is fatal"    "$(run 'echo $(if true)')"    "2|"
check "parse error in \$() stops the rest of the script" \
    "$(run 'echo $(if true)
echo after')" "2|"
check "unterminated quote inside backticks is fatal" \
    "$("$MB" -c 'echo `echo "`' 2>/dev/null; echo "st=$?")" "st=2"
check "a command substitution that merely FAILS is not fatal" \
    "$(run 'echo "[$(false)]"; echo alive')" "0|[]
alive"

# -----------------------------------------------------------------------
# 5. Unknown parameter-expansion operator
# -----------------------------------------------------------------------
check "\${a&} is a bad substitution"    "$(run 'a=x; echo ${a&}')"    "2|"
check "\${a^^} is a bad substitution"   "$(run 'a=x; echo ${a^^}')"   "2|"
check "the operators silex does support still work" \
    "$("$MB" -c 'a=abc; echo ${a#a} ${a%c} ${#a} ${a:-d} ${a:+y} ${b:-d} ${a:1}')" \
    "bc ab 3 abc y d bc"

# -----------------------------------------------------------------------
# 6. Listings must be suitable for re-input
# -----------------------------------------------------------------------
check "export -p round-trips a value containing quotes" \
    "$("$MB" -c 'export V="a'"'"'b'"'"'c"; eval "$(export -p)"; printf "%s" "$V"')" \
    "a'b'c"
check "set round-trips a value containing quotes" \
    "$("$MB" -c 'V="a'"'"'b"; eval "$(set | grep "^V=")"; printf "%s" "$V"')" \
    "a'b"
check "trap listing round-trips an action containing quotes" \
    "$("$MB" -c 'trap "echo '"'"'bye'"'"'" USR1; trap' | sed -n 's/^trap -- //p')" \
    "'echo '\\''bye'\\''' USR1"
check "alias listing round-trips a value containing quotes" \
    "$("$MB" -c "alias q=\"echo 'hi'\"; alias q")" \
    "alias q='echo '\\''hi'\\'''"

# -----------------------------------------------------------------------
echo ""
echo "syntax error tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
