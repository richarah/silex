#!/bin/sh
# tests/unit/shell/test_control.sh — control structure tests for silex shell
# chmod +x tests/unit/shell/test_control.sh
# Usage: ./test_control.sh [path/to/silex]

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

# ===========================================================================
# if / then / elif / else / fi
# ===========================================================================

got=$("$MB" -c 'if true; then echo yes; fi')
check "if true: then branch taken" "$got" "yes"

got=$("$MB" -c 'if false; then echo yes; else echo no; fi')
check "if false: else branch taken" "$got" "no"

got=$("$MB" -c 'if false; then echo a; elif true; then echo b; else echo c; fi')
check "elif: second branch taken" "$got" "b"

got=$("$MB" -c 'if false; then echo a; elif false; then echo b; else echo c; fi')
check "elif false: else branch taken" "$got" "c"

got=$("$MB" -c '
X=5
if [ "$X" -gt 10 ]; then
    echo big
elif [ "$X" -gt 3 ]; then
    echo medium
else
    echo small
fi')
check "if/elif/else with test -gt" "$got" "medium"

# if exit code
"$MB" -c 'if true; then true; fi'
check_exit "if true: exit 0" "$?" "0"

"$MB" -c 'if false; then true; fi'
check_exit "if false (no else): exit 0" "$?" "0"

"$MB" -c 'if false; then true; else false; fi'
check_exit "if false with else false: exit 1" "$?" "1"

# Nested if
got=$("$MB" -c '
if true; then
    if true; then
        echo nested_yes
    fi
fi')
check "nested if/then" "$got" "nested_yes"

# ===========================================================================
# while / do / done
# ===========================================================================

got=$("$MB" -c '
I=0
while [ "$I" -lt 3 ]; do
    echo $I
    I=$((I + 1))
done')
check "while loop: count 0 1 2" "$got" "$(printf '0\n1\n2')"

# while false: body never executes
got=$("$MB" -c 'while false; do echo never; done')
check "while false: body not executed" "$got" ""

# while exit code: last iteration exit
"$MB" -c 'I=0; while [ "$I" -lt 1 ]; do I=$((I+1)); done'
check_exit "while loop exit: 0 when condition eventually false" "$?" "0"

# ===========================================================================
# until / do / done
# ===========================================================================

got=$("$MB" -c '
I=0
until [ "$I" -ge 3 ]; do
    echo $I
    I=$((I + 1))
done')
check "until loop: count 0 1 2" "$got" "$(printf '0\n1\n2')"

# until true: body never executes
got=$("$MB" -c 'until true; do echo never; done')
check "until true: body not executed" "$got" ""

# ===========================================================================
# for / in / do / done
# ===========================================================================

got=$("$MB" -c 'for X in a b c; do echo $X; done')
check "for/in: iterate a b c" "$got" "$(printf 'a\nb\nc')"

# for with glob
TMPDIR_CTRL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_CTRL"' EXIT INT TERM
touch "$TMPDIR_CTRL/f1.txt" "$TMPDIR_CTRL/f2.txt" "$TMPDIR_CTRL/f3.txt"
got=$("$MB" -c "for F in $TMPDIR_CTRL/*.txt; do echo \$(basename \$F); done" | sort)
check "for/in glob: iterate .txt files" "$got" "$(printf 'f1.txt\nf2.txt\nf3.txt')"

# for with command substitution
got=$("$MB" -c 'for X in $(echo a b c); do echo $X; done')
check "for/in with command substitution" "$got" "$(printf 'a\nb\nc')"

# for with no list: iterate positional params
got=$("$MB" -c 'set -- p q r; for X; do echo $X; done')
check "for (no in): iterates positional params" "$got" "$(printf 'p\nq\nr')"

# ===========================================================================
# case / in / esac
# ===========================================================================

got=$("$MB" -c '
V=hello
case "$V" in
    hello) echo matched_hello ;;
    world) echo matched_world ;;
    *)     echo matched_default ;;
esac')
check "case: first pattern matched" "$got" "matched_hello"

got=$("$MB" -c '
V=world
case "$V" in
    hello) echo matched_hello ;;
    world) echo matched_world ;;
    *)     echo matched_default ;;
esac')
check "case: second pattern matched" "$got" "matched_world"

got=$("$MB" -c '
V=other
case "$V" in
    hello) echo matched_hello ;;
    world) echo matched_world ;;
    *)     echo matched_default ;;
esac')
check "case: wildcard * matched" "$got" "matched_default"

# case with | alternation
got=$("$MB" -c '
V=yes
case "$V" in
    yes|y|Y) echo affirmative ;;
    no|n|N)  echo negative ;;
esac')
check "case with | alternation: yes" "$got" "affirmative"

got=$("$MB" -c '
V=n
case "$V" in
    yes|y|Y) echo affirmative ;;
    no|n|N)  echo negative ;;
esac')
check "case with | alternation: n" "$got" "negative"

# case with glob pattern
got=$("$MB" -c '
V=foobar
case "$V" in
    foo*)  echo starts_foo ;;
    *bar)  echo ends_bar ;;
    *)     echo other ;;
esac')
check "case glob: foo* matched before *bar" "$got" "starts_foo"

# case exit code
"$MB" -c 'case x in x) true ;; esac'
check_exit "case: matched and last cmd true -> 0" "$?" "0"

"$MB" -c 'case x in y) echo no ;; esac'
check_exit "case: no match -> exit 0" "$?" "0"

# ===========================================================================
# break / continue
# ===========================================================================

got=$("$MB" -c '
for I in 1 2 3 4 5; do
    if [ "$I" -eq 3 ]; then break; fi
    echo $I
done')
check "break: stops loop at I=3" "$got" "$(printf '1\n2')"

got=$("$MB" -c '
for I in 1 2 3 4 5; do
    if [ "$I" -eq 3 ]; then continue; fi
    echo $I
done')
check "continue: skips I=3" "$got" "$(printf '1\n2\n4\n5')"

# break with level
got=$("$MB" -c '
for I in 1 2; do
    for J in a b; do
        if [ "$I" = "2" ] && [ "$J" = "a" ]; then break 2; fi
        echo "$I$J"
    done
done')
check "break 2: breaks both loops" "$got" "$(printf '1a\n1b')"

# continue with level
got=$("$MB" -c '
for I in 1 2 3; do
    for J in a b; do
        if [ "$J" = "b" ]; then continue 2; fi
        echo "$I$J"
    done
done')
check "continue 2: continues outer loop" "$got" "$(printf '1a\n2a\n3a')"

# break in while
got=$("$MB" -c '
I=0
while true; do
    I=$((I + 1))
    if [ "$I" -ge 3 ]; then break; fi
    echo $I
done')
check "break in while loop" "$got" "$(printf '1\n2')"

# ===========================================================================
# Exit codes from control structures
# ===========================================================================

"$MB" -c 'if true; then true; fi'
check_exit "control structure exit: if true -> 0" "$?" "0"

"$MB" -c 'while false; do :; done'
check_exit "while false exit: 0" "$?" "0"

"$MB" -c 'for x in a; do false; done'
check_exit "for loop: last iteration exit propagated" "$?" "1"

# ===========================================================================
# set -e: errexit exemptions
# ===========================================================================

# set -e should NOT trigger in if condition
got=$("$MB" -c 'set -e; if false; then echo WRONG; fi; echo PASS')
check "set -e: if condition failure doesn't exit shell" "$got" "PASS"

# set -e should NOT trigger on left side of &&
got=$("$MB" -c 'set -e; false && true; echo PASS')
check "set -e: left of && doesn't trigger exit" "$got" "PASS"

# set -e should NOT trigger on left side of ||
got=$("$MB" -c 'set -e; false || echo FALLBACK; echo PASS')
check "set -e: left of || doesn't trigger exit" "$got" "$(printf 'FALLBACK\nPASS')"

# set -e SHOULD trigger on a plain failing command
"$MB" -c 'set -e; false; echo SHOULD_NOT_PRINT' 2>/dev/null
check_exit "set -e: plain false triggers exit" "$?" "1"

# set -e should NOT trigger for while condition
got=$("$MB" -c 'set -e; while false; do echo NEVER; done; echo PASS')
check "set -e: while condition failure doesn't exit" "$got" "PASS"

# set -e should NOT trigger for negated command
got=$("$MB" -c 'set -e; ! false; echo PASS')
check "set -e: negated false doesn't trigger exit" "$got" "PASS"

# --- case-pattern quoting (POSIX 2.13): quoted metacharacters are literal ------
# Regression for the FTL_SQBKSL modernish blocker and the general bug where
# expand_word() quote-removed the pattern, letting quoted '*'/'?'/'['/'\' reach
# fnmatch as active wildcards. Every expected value matches dash.

# Quoted metacharacters must NOT act as wildcards.
check "case: quoted * is literal (no match)" \
    "$("$MB" -c 'case a in "*") echo M;; *) echo N;; esac')" "N"
check "case: quoted * matches literal *" \
    "$("$MB" -c 'case "*" in "*") echo M;; *) echo N;; esac')" "M"
check "case: quoted a* is literal (no match)" \
    "$("$MB" -c 'case abc in "a*") echo M;; *) echo N;; esac')" "N"
check "case: quoted ? is literal (no match)" \
    "$("$MB" -c 'case x in "?") echo M;; *) echo N;; esac')" "N"
check "case: quoted [fb] is literal (no match)" \
    "$("$MB" -c 'case foo in "[fb]oo") echo M;; *) echo N;; esac')" "N"
check "case: backslash-escaped * is literal" \
    "$("$MB" -c 'case abc in a\*) echo M;; *) echo N;; esac')" "N"
check "case: backslash-escaped * matches literal" \
    "$("$MB" -c 'case "a*" in a\*) echo M;; *) echo N;; esac')" "M"

# Unquoted metacharacters -- literal or from expansion -- stay active.
check "case: unquoted * is an active wildcard" \
    "$("$MB" -c 'case abc in a*) echo M;; *) echo N;; esac')" "M"
check "case: unquoted expansion \$p=* is active" \
    "$("$MB" -c 'p="*"; case a in $p) echo M;; *) echo N;; esac')" "M"

# A quoted expansion result is literal; an unquoted one is active.
check "case: quoted \"\$x\"=* is literal (no match)" \
    "$("$MB" -c 'x="*"; case abc in "$x") echo M;; *) echo N;; esac')" "N"
check "case: unquoted \$x=a* is active (match)" \
    "$("$MB" -c 'x="a*"; case abc in $x) echo M;; *) echo N;; esac')" "M"

# modernish FTL_SQBKSL: a backslash-newline in a single-quoted subject must
# match a pattern with an escaped backslash and a quoted newline.
check "case: FTL_SQBKSL backslash pattern matches" \
    "$("$MB" -c 'v="\\"; case "$v" in "\\") echo M;; *) echo N;; esac')" "M"

# --- set -o long option names --------------------------------------------------
# modernish's debug path uses `set -o xtrace`; the long spellings must map to the
# same flags as the short forms.
check "set -o errexit: false exits" \
    "$("$MB" -c 'set -o errexit; false; echo SHOULD_NOT_PRINT' 2>/dev/null; echo "rc=$?")" "rc=1"
check "set -o noglob then +o noglob toggles" \
    "$("$MB" -c 'set -o noglob; set +o noglob; echo ok')" "ok"
"$MB" -c 'set -o nounset; echo "$undefined_var"' 2>/dev/null
check_exit "set -o nounset: unset var errors" "$?" "1"
"$MB" -c 'set -o bogusname' 2>/dev/null
check_exit "set -o bogus: rejected" "$?" "1"

# --- xtrace (set -x) actually prints the command, to stderr, post-expansion ----
# opt_x was set but never consulted, so `set -x` traced nothing.
check "xtrace: traces a command to stderr" \
    "$("$MB" -c 'set -x; echo hi' 2>&1 1>/dev/null)" "+ echo hi"
check "xtrace: honors PS4" \
    "$("$MB" -c 'PS4="# "; set -x; echo hi' 2>&1 1>/dev/null)" "# echo hi"
check "xtrace: off by default (no trace)" \
    "$("$MB" -c 'echo hi' 2>&1 1>/dev/null)" ""
# `set +x` is itself traced (it runs while -x is still on), then echo is not.
check "xtrace: set +x stops tracing after itself" \
    "$("$MB" -c 'set -x; set +x; echo hi' 2>&1 1>/dev/null)" "+ set +x"

# --- incomplete compound commands are syntax errors (rc 2), not silent no-ops --
# The parser accepted a bare `(`, `if`, `while`, etc. as a clean empty parse
# returning 0, so `eval '('` succeeded where it must fail (modernish FTL_EVALERR).
for kw in '(' 'if' 'while' 'until' 'for' 'case' '{'; do
    "$MB" -c "$kw" 2>/dev/null
    check_exit "parse: bare '$kw' is a syntax error" "$?" "2"
done
# Valid forms of the same constructs still work.
check "parse: valid subshell runs" "$("$MB" -c '(echo hi)')" "hi"
check "parse: valid brace group runs" "$("$MB" -c '{ echo hi; }')" "hi"
check "parse: valid if runs" "$("$MB" -c 'if true; then echo yes; fi')" "yes"

# --- return / break / continue inside eval act on the enclosing function/loop --
# eval is transparent to flow control; the sentinel used to leak out and exit
# the shell (modernish FTL_EVALRET / FTL_EVALCOBR).
check "eval: return exits the enclosing function (not the shell)" \
    "$("$MB" -c 'f() { eval "return 3"; echo AFTER; }; f; echo "rc=$?"')" "rc=3"
check "eval: continue affects the enclosing loop" \
    "$("$MB" -c 'for i in 1 2 3; do eval continue; echo NO; done; echo done')" "done"
check "eval: break affects the enclosing loop" \
    "$("$MB" -c 'for i in 1 2 3; do eval break; echo NO; done; echo done')" "done"

# --- `command` demotes a special builtin so its error does not exit the shell --
# modernish FTL_CMDSPEXIT: `command unset` of a readonly var must not exit.
check "command: unset of readonly does not exit the shell" \
    "$("$MB" -c 'readonly RO=1; command unset -v RO 2>/dev/null; echo AFTER')" "AFTER"
"$MB" -c 'readonly RO=1; unset -v RO 2>/dev/null; echo SHOULD_NOT' >/dev/null 2>&1
check_exit "plain unset of readonly still exits (POSIX special builtin)" "$?" "1"

echo ""
echo "control structure tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
