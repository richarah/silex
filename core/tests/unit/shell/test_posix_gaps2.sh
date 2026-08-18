#!/bin/sh
# tests/unit/shell/test_posix_gaps2.sh
#
# Regression tests for the POSIX gaps closed on 2026-08-18. As with
# test_posix_gaps.sh, each is a place where silex quietly did something other
# than dash and bash, with nothing in the script able to see it.
#
#   1. An assignment prefix on a SPECIAL builtin persists as a shell variable
#      but is NOT exported. silex exported it, so `IFS=: readonly x` (and any
#      other prefix on a special builtin) leaked into the environment of every
#      later child. The temporary export exists only so a builtin that goes on
#      to exec something can be reached through it; it has to be undone even
#      where the VALUE stays. (Oils builtin-special 3.)
#
#   2. PWD and OLDPWD are exported, not merely set. silex set them, so they
#      reached a child only when the parent's environment had happened to
#      supply them -- and OLDPWD, which nothing inherits, never did.
#      (Oils builtin-cd 5, vars-special 1.)
#
#   3. An unquoted command substitution in a here-document delimiter is a
#      syntax error. The delimiter takes quote removal and nothing else, so
#      `<<$(a)` names a delimiter no line can be written to match. dash and
#      mksh reject it; silex accepted it and read the body. (Oils here-doc 6.)
#
# The `2` in the filename is only to keep it apart from the earlier batch.

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

runc() {
    out=$("$MB" -c "$1" 2>/dev/null)
    printf '%s|%s' "$?" "$out"
}

# -----------------------------------------------------------------------
# 1. Assignment prefix on a special builtin: value persists, export does not
# -----------------------------------------------------------------------

# POSIX 2.9.1: the assignment stays in effect after the special builtin.
check "special builtin prefix: the value persists in the shell" \
      "$(runc 'pre=v readonly x=x; echo "[$pre]"')" \
      "0|[v]"

# ...but a plain assignment does not export, and writing it in front of a
# command does not change that. This is the half that was wrong.
#
# Phrased as an if/else rather than `grep -c` so the assertion is about the
# environment and not about grep's exit status, which is 1 for a zero count.
check "special builtin prefix: the value is NOT exported" \
      "$(runc 'pre=v readonly x=x
               if env | grep -q "^pre="; then echo LEAKED; else echo clean; fi')" \
      "0|clean"

# The case as Oils states it: the next command is an exec, so the child shows
# what actually reached the environment.
check "special builtin prefix: a later exec does not see it" \
      "$(runc 'pre1=pre1 readonly x=x
pre2=pre2 exec sh -c "echo pre1=\$pre1 x=\$x pre2=\$pre2"')" \
      "0|pre1= x= pre2=pre2"

# A prefix on a NON-special builtin is undone completely, as before.
check "regular builtin prefix: the value is restored" \
      "$(runc 'pre=outer; pre=inner command true; echo "[$pre]"')" \
      "0|[outer]"

# The export must still be visible FOR THE DURATION, or `PATH=... command cmd`
# stops finding cmd through the PATH it was given. This is what the temporary
# export is for, and the fix must not take it away.
check "prefix export reaches the command it prefixes" \
      "$(runc 'PATH=/usr/bin command env | grep -c "^PATH=/usr/bin$"')" \
      "0|1"

# `export` asked for the flag, so it keeps it.
check "x=1 export x still exports" \
      "$(runc 'x=1 export x; env | grep "^x="')" \
      "0|x=1"

# A name that was ALREADY exported keeps its export flag afterwards.
check "an already-exported name stays exported" \
      "$(runc 'export e=one; e=two readonly y=y; env | grep "^e="')" \
      "0|e=two"

# -----------------------------------------------------------------------
# 2. PWD and OLDPWD are exported
# -----------------------------------------------------------------------

check "cd exports OLDPWD" \
      "$(runc 'cd /; cd /tmp; env | grep "^OLDPWD="')" \
      "0|OLDPWD=/"

check "cd exports PWD" \
      "$(runc 'cd /tmp; env | grep "^PWD="')" \
      "0|PWD=/tmp"

check "OLDPWD still drives cd -" \
      "$(runc 'cd /; cd /tmp; cd - >/dev/null; pwd')" \
      "0|/"

# PWD is exported even when the shell did not inherit it -- the case a `set`
# of PWD alone cannot cover, because an inherited PWD is already exported and
# hides the bug.
check "PWD is exported without being inherited" \
      "$(env -u PWD "$MB" -c \
         'if env | grep -q "^PWD="; then echo exported; else echo MISSING; fi' \
         2>/dev/null)" \
      "exported"

# -----------------------------------------------------------------------
# 3. Command substitution in a here-document delimiter
# -----------------------------------------------------------------------

check "here-doc delimiter: \$( ) is a syntax error" \
      "$(run 'cat <<$(a)
here
$(a)')" \
      "2|"

# Only `$(` is rejected -- dash accepts every other form, and so must silex.
check "here-doc delimiter: \$X is fine" \
      "$(run 'cat <<$X
here
$X')" \
      "0|here"

check "here-doc delimiter: \${X} is fine" \
      "$(run 'cat <<${X}
here
${X}')" \
      "0|here"

check "here-doc delimiter: backticks are fine" \
      "$(run 'cat <<`a`
here
`a`')" \
      "0|here"

# Quoting makes it ordinary text again.
check "here-doc delimiter: quoted \$( ) is fine" \
      "$(run "cat <<'\$(a)'
here
\$(a)")" \
      "0|here"

# The rejection must not fire on a command substitution anywhere ELSE on the
# line -- only in the delimiter word itself.
check "here-doc: \$( ) elsewhere on the line is unaffected" \
      "$(run 'cat <<EOF > /dev/null; echo "[$(echo ok)]"
body
EOF')" \
      "0|[ok]"

# -----------------------------------------------------------------------
# 4. Tilde expansion inside a ${var-word} in an ASSIGNMENT
# -----------------------------------------------------------------------
#
# An assignment RHS expands a tilde at the start and after every unquoted
# colon (PATH=~/bin:~/sbin), and POSIX applies that to the whole RHS --
# including text a ${...-word} contributes. silex expanded neither tilde in
# `x=${undef-~:~}` and printed `~:~`. (Oils tilde 11.)
#
# Both were lost together: expand_tilde() was called with in_assignment=0, so
# the colon was not a delimiter, the "username" became `:~`, no such user
# exists, and the word came back untouched -- taking the LEADING tilde with it.

check "tilde: a \${v-word} in an assignment expands after a colon" \
      "$(HOME=/home/bar "$MB" -c 'x=~:${undef-~:~}; echo $x' 2>/dev/null)" \
      "/home/bar:/home/bar:/home/bar"

check "tilde: the leading one alone still works" \
      "$(HOME=/home/bar "$MB" -c 'x=${undef-~}; echo $x' 2>/dev/null)" \
      "/home/bar"

# OUTSIDE an assignment a colon is NOT a delimiter, so only a leading tilde
# expands and `~:~` stays literal. dash agrees; this is the guard that the fix
# is scoped to assignment context.
check "tilde: outside an assignment the colon is not a delimiter" \
      "$(HOME=/home/bar "$MB" -c 'echo ${undef-~:~}' 2>/dev/null)" \
      "~:~"

check "tilde: a quoted word is still literal" \
      "$(HOME=/home/bar "$MB" -c 'echo "${undef-~}"' 2>/dev/null)" \
      "~"

# The plain assignment forms must be untouched.
check "tilde: PATH-style assignment still expands each segment" \
      "$(HOME=/home/bar "$MB" -c 'x=~/bin:~/sbin; echo $x' 2>/dev/null)" \
      "/home/bar/bin:/home/bar/sbin"

# A colon inside an expansion is still not a segment delimiter for the RHS
# split itself -- the reason expand_word_assign skips ${...} whole.
check "tilde: \${u:-x} in an assignment is not cut at its colon" \
      "$(HOME=/home/bar "$MB" -c 'V=${u:-x}; echo $V' 2>/dev/null)" \
      "x"

check "tilde: a non-tilde word with a colon is unchanged" \
      "$(HOME=/home/bar "$MB" -c 'x=${undef-a:b}; echo $x' 2>/dev/null)" \
      "a:b"

# smoosh semantics.var.format.tilde -- := assigns $HOME.
check "tilde: \${x:=~} assigns HOME" \
      "$(HOME=/home/bar "$MB" -c ': ${x:=~}; echo $x' 2>/dev/null)" \
      "/home/bar"

# -----------------------------------------------------------------------
# 4. `read` without -r resolves backslash escapes, and an escaped IFS
#    delimiter stops delimiting
# -----------------------------------------------------------------------
# silex read the line keeping every `\` and its successor in the buffer, then
# split that buffer without ever looking at the backslashes again. So the
# escape did neither of the two things POSIX asks of it: the backslash stayed
# in the value, and an escaped delimiter still cut the field in two.
# (Oils builtin-read 24.)

check "read: an escaped IFS delimiter neither splits nor keeps its backslash" \
      "$("$MB" -c 'IFS=: read a b <<'"'"'EOF'"'"'
x\:y:z
EOF
printf "[%s|%s]" "$a" "$b"' 2>/dev/null)" \
      "[x:y|z]"

check "read: a doubled backslash becomes one" \
      "$("$MB" -c 'read p q <<'"'"'EOF'"'"'
a\\b c
EOF
printf "[%s|%s]" "$p" "$q"' 2>/dev/null)" \
      "[a\\b|c]"

# -r is the control: it must do neither of the above.
check "read -r: backslashes are literal and an escaped delimiter still splits" \
      "$("$MB" -c 'IFS=: read -r a b <<'"'"'EOF'"'"'
x\:y:z
EOF
printf "[%s|%s]" "$a" "$b"' 2>/dev/null)" \
      "[x\\|y:z]"

check "read: a backslash-newline continues the line" \
      "$("$MB" -c 'read m n <<'"'"'EOF'"'"'
one\
two three
EOF
printf "[%s|%s]" "$m" "$n"' 2>/dev/null)" \
      "[onetwo|three]"

# A backslash with nothing after it escapes nothing, so it is dropped -- dash
# and bash both leave t=trail. Status is 1 because the input hit EOF with no
# newline.
check "read: a trailing backslash at EOF is discarded" \
      "$(printf 'trail\\' | "$MB" -c 'read t; printf "%s|%s" "$?" "$t"' \
         2>/dev/null)" \
      "1|trail"

# The Oils case in full: leading spaces are NOT trimmed (space is not in IFS),
# the escaped colon is literal, and the continuation glues `d` to `  e`.
check "read: Oils builtin-read 24, escapes and IFS=: together" \
      "$("$MB" -c 'IFS=: read a b c d <<'"'"'EOF'"'"'
  \\a :b\: c:d\
  e
EOF
printf "[%s|%s|%s|%s]" "$a" "$b" "$c" "$d"' 2>/dev/null)" \
      "[  \\a |b: c|d  e|]"

# -----------------------------------------------------------------------

echo ""
echo "posix gap tests (2): $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
