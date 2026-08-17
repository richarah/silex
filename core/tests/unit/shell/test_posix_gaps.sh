#!/bin/sh
# tests/unit/shell/test_posix_gaps.sh
#
# Regression tests for the POSIX gaps closed in the 59 -> N differential run.
# Each case is a place where silex quietly did something OTHER than what dash
# and bash do, and where the difference was invisible from the script's side.
#
#   1. Grammar: `(` after an assignment or command word is a syntax error, and
#      an assignment prefix ends reserved-word recognition. `a=(1 2 3)` used to
#      run the array elements as commands.
#   2. A function name is a literal, so it may not carry an expansion:
#      `$foo-bar() { ...; }` defined a function no call could ever name.
#   3. A reserved word may still name the `for` variable: `for in in a b c`.
#   4. `local NAME` twice must not wipe the value the first one gave it.
#   5. `type` knows about aliases and functions, and calls out the special
#      builtins (they persist assignments and their errors are fatal).
#   6. Field splitting acts on the RESULTS of expansions, not on the word's own
#      literal text, and unquoted $@/$* are separated by IFS's first character.
#   7. Flow control at the top level of a script has nothing to act on: the
#      sentinel used to escape as $? (`sh -c 'return'` exited 234).
#   8. A command that exists but cannot be executed is 126, not 127.
#   9. A syntax error exits 2 even when an EXIT trap runs.
#  10. A failed redirection means the command never runs -- including its name
#      lookup, which was still reporting "command not found" and exiting 127.
#  11. `n>&n` duplicates a descriptor onto itself: a no-op, even when closed.
#  12. Inside "...", `\"` is unescaped before a backtick body is run.
#  13. A here-document delimiter is the word after quote removal.
#  14. An IFS that is set but EMPTY joins "$*" with nothing, not with a space.
#  15. The scan for a ${...}'s closing brace honours '...' even inside "...".
#  16. An applet that hits a closed pipe reports 141 without taking the shell
#      down with it.
#  17. An alias is substituted for the command word even when redirections
#      precede it.
#
# Usage: ./test_posix_gaps.sh [path/to/silex]

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
# wording is not the contract, the status and the output are.
run() {
    out=$(printf '%s\n' "$1" | "$MB" 2>/dev/null)
    printf '%s|%s' "$?" "$out"
}

# Same, but for code given to -c (so $0/positional handling is exercised too).
runc() {
    out=$("$MB" -c "$1" 2>/dev/null)
    printf '%s|%s' "$?" "$out"
}

# -----------------------------------------------------------------------
# 1. `(` cannot follow an assignment or a command word
# -----------------------------------------------------------------------
check "a=(1 2 3) is a syntax error"        "$(run 'a=(1 2 3)')"          "2|"
check "a= (1 2) is a syntax error"         "$(run 'a= (1 2)')"           "2|"
check "assignment prefix then ( is an error" \
    "$(run 'A=a B=(b b) echo hi')" "2|"
check "echo a(b) is a syntax error"        "$(run 'echo a(b)')"          "2|"
check "a subshell after a separator still parses" \
    "$(run 'x=1; (echo sub)')" "0|sub"
check "an assignment prefix ends keyword recognition" \
    "$(run 'FOO=bar for')" "127|"
check "a real for loop is unaffected" \
    "$(run 'for i in a b; do echo $i; done')" "0|a
b"

# -----------------------------------------------------------------------
# 2. A function name may not carry an expansion
# -----------------------------------------------------------------------
check "function name with \$ is an error"  "$(run '$foo-bar() { ls ; }')"       "2|"
check "function name with a comsub is an error" \
    "$(run 'foo-$(echo hi)() { ls ; }')" "2|"
check "an ordinary function name still works" \
    "$(run 'foo-bar() { echo ok; }; foo-bar')" "0|ok"

# -----------------------------------------------------------------------
# 3. A reserved word can name the loop variable
# -----------------------------------------------------------------------
check "for in in a b c" \
    "$(run 'for in in a b c; do echo $in; done; echo done=$in')" "0|a
b
c
done=c"
check "for with no variable is still an error"  "$(run 'for')"        "2|"
check "for with a non-name is still an error"   "$(run 'for i.j in a; do :; done')" "2|"

# -----------------------------------------------------------------------
# 4. Declaring the same name local twice keeps the value
# -----------------------------------------------------------------------
check "local x=v; local x keeps v" \
    "$(run 'f(){ local y=a; local y; echo "[$y]"; }; f')" "0|[a]"
check "local x= still clears it" \
    "$(run 'f(){ local y=a; local y=; echo "[$y]"; }; f')" "0|[]"

# -----------------------------------------------------------------------
# 5. type reports aliases, functions and special builtins
# -----------------------------------------------------------------------
check "type finds a function" \
    "$(runc 'f(){ :; }; type f')" "0|f is a shell function"
check "type finds an alias" \
    "$(runc 'alias ll="ls -l"; type ll')" "0|ll is an alias for ls -l"
check "type calls out a special builtin" \
    "$(runc 'type eval')" "0|eval is a special shell builtin"
check "type leaves a regular builtin alone" \
    "$(runc 'type cd')" "0|cd is a shell builtin"

# -----------------------------------------------------------------------
# 6. Field splitting: literal text is not split; $@/$* use IFS[0]
# -----------------------------------------------------------------------
# A literal IFS character in the WORD is not a separator: only the results of
# expansions are split. `${w}x` used to lose the trailing x entirely, because
# it started an empty field and empty fields are dropped.
check "a literal IFS char after an expansion stays in the field" \
    "$(runc 'IFS=x; w=AxB; echo ${w}x')" "0|A Bx"
check "a literal IFS char after \${v:-word}" \
    "$(runc 'IFS=x; v=; echo ${v:-AxBxC}x')" "0|A B Cx"
# ...but the WORD of the substitution is part of the result, so it IS split.
check "the word of \${v:-word} is still split" \
    "$(runc 'IFS=x; v=; echo ${v:-AxBxC}')" "0|A B C"
check "quoting the word suppresses that split" \
    "$(runc 'IFS=x; v=; echo ${v:-"AxBxC"}x')" "0|AxBxCx"
# Unquoted $@/$* separate on IFS's FIRST character, not on a space that IFS
# may not even contain.
check "unquoted \$@ with a non-space IFS" \
    "$(runc 'IFS=zx; set -- "a b" c; for i in $@; do echo -$i-; done')" "0|-a b-
-c-"
check "unquoted \$* with a non-space IFS" \
    "$(runc 'IFS=zx; set -- "a b" c; for i in $*; do echo -$i-; done')" "0|-a b-
-c-"
check "IFS whitespace still collapses around the join" \
    "$(runc 'set " abc " " def ghi "; IFS=" "; set $*; echo $#:$1:$2:$3')" "0|3:abc:def:ghi"

# -----------------------------------------------------------------------
# 7. Flow control at the top level
# -----------------------------------------------------------------------
check "top-level break is a no-op"     "$(runc 'break; echo break=$?')"       "0|break=0"
check "top-level continue is a no-op"  "$(runc 'continue; echo continue=$?')" "0|continue=0"
check "top-level return ends the script" "$(runc 'return; echo NOPE')"        "0|"
check "top-level return from stdin ends the script" \
    "$(run 'return
echo NOPE')" "0|"
# ...while a sentinel that DOES have somewhere to go still goes there.
check "eval return still returns from the function" \
    "$(runc 'f(){ eval "return 3"; echo NOPE; }; f; echo got=$?')" "0|got=3"
check "eval break still breaks the loop" \
    "$(runc 'for x in a b c; do echo $x; eval break; done')" "0|a"

# -----------------------------------------------------------------------
# 8. 126 for a command that cannot be executed
# -----------------------------------------------------------------------
tmpd=$(mktemp -d) || exit 1
: > "$tmpd/plain"
chmod 644 "$tmpd/plain"
check "a non-executable file is 126" "$(runc "$tmpd/plain")"        "126|"
check "a missing file is still 127"  "$(runc "$tmpd/nosuchfile")"   "127|"
rm -rf "$tmpd"

# -----------------------------------------------------------------------
# 9. A syntax error exits 2 even with an EXIT trap
# -----------------------------------------------------------------------
check "an EXIT trap does not mask a syntax error" \
    "$(run 'trap "echo FAILED" EXIT
for')" "2|FAILED"

# -----------------------------------------------------------------------
# 10. A failed redirection stops the command before its name is looked up
# -----------------------------------------------------------------------
check "a failed redirect on an unknown command is not 127" \
    "$(runc 'nosuchcmd-zz < /nonexistent-zz; echo st=$?')" "0|st=1"
check "an unknown command with no redirect is still 127" \
    "$(runc 'nosuchcmd-zz; echo st=$?')" "0|st=127"

# -----------------------------------------------------------------------
# 11. n>&n is a no-op even when n is closed
# -----------------------------------------------------------------------
check ": 3>&3 succeeds with fd 3 closed" "$(runc ': 3>&3; echo hello')" "0|hello"
check ": 3<&3 succeeds with fd 3 closed" "$(runc ': 3<&3; echo hello')" "0|hello"
# ...a genuinely different descriptor is still an error, and the command
# it was attached to does not run.
check "a real bad fd is still an error" \
    "$(runc 'echo NOPE 4>&3; echo st=$?')" "0|st=1"

# -----------------------------------------------------------------------
# 12. Backslash-quote inside a backtick inside double quotes
# -----------------------------------------------------------------------
check 'backtick in "..." unescapes \"' \
    "$(runc 'echo "x `echo \"hi\"`"')" '0|x hi'
check '$( ) in "..." does not' \
    "$(runc 'echo "x $(echo \"hi\")"')" '0|x "hi"'

# -----------------------------------------------------------------------
# 13. The here-document delimiter is the word after quote removal
# -----------------------------------------------------------------------
check "a partly quoted delimiter" \
    "$(run "cat <<'EOF'\"2\"
one
EOF2")" "0|one"
check "a quoted delimiter still suppresses expansion" \
    "$(run 'V=w; cat <<"D"
$V
D')" '0|$V'
check "an unquoted delimiter still expands" \
    "$(run 'V=w; cat <<D
$V
D')" "0|w"

# -----------------------------------------------------------------------
# 14. An IFS that is set but empty joins with nothing
# -----------------------------------------------------------------------
check '"$*" with an empty IFS joins with nothing' \
    "$(runc 'set -- a b c; IFS=; echo "[$*]"')" "0|[abc]"
check '${*:-word} sees the empty join as null' \
    "$(runc 'set -- "" ""; IFS=; echo "argv=${*:-minus}"')" "0|argv=minus"
check "an unset IFS still joins with a space" \
    "$(runc 'set -- a b c; unset IFS; echo "[$*]"')" "0|[a b c]"

# -----------------------------------------------------------------------
# 15. The ${...} scan honours '...' inside "..."
# -----------------------------------------------------------------------
check "a quoted brace in a strip pattern" \
    "$(runc "var='}'; echo 1 \"\${var#'}'}\"")" "0|1 "
check "an escaped brace in a strip pattern" \
    "$(runc 'var="}"; echo 2 "${var#\}}"')" "0|2 "
check "a brace that is data is left alone" \
    "$(runc 'var="}"; echo 3 "${var#}}"')" "0|3 }}"

# -----------------------------------------------------------------------
# 16. An applet on a closed pipe reports 141 and the shell lives
# -----------------------------------------------------------------------
check "a builtin applet does not die of SIGPIPE in place" \
    "$(runc '((cat </dev/zero; echo $? >&7) | true) 7>&1')" "0|141"
if [ -x /usr/bin/cat ]; then
    check "the external it stands in for agrees" \
        "$(runc '((/usr/bin/cat </dev/zero; echo $? >&7) | true) 7>&1')" "0|141"
fi

# -----------------------------------------------------------------------
# 17. An alias is substituted after a redirection prefix
# -----------------------------------------------------------------------
tmpf=$(mktemp) || exit 1
# The alias has to be DEFINED before the line using it is parsed, hence the
# newline rather than a `;` (every shell works this way).
check "a redirect before the command word does not hide the alias" \
    "$(runc "alias e_=echo
>$tmpf e_ 1
cat $tmpf")" "0|1"
check "an alias name in argument position stays literal" \
    "$(runc 'alias e_=echo
echo e_')" "0|e_"
check "a self-referential alias expands once" \
    "$(runc 'alias e_=e_
e_ 2>/dev/null
echo done=$?')" "0|done=127"
rm -f "$tmpf"

# -----------------------------------------------------------------------
echo ""
echo "posix gap tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
