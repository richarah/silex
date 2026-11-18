#!/bin/sh
# tests/unit/shell/test_variables.sh — shell variable tests for silex
# chmod +x tests/unit/shell/test_variables.sh
# Usage: ./test_variables.sh [path/to/silex]

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
# Use escaped $ so the outer shell does NOT expand ${NOT_EXPORTED:-absent};
# the child sh receives it literally and sees an unset variable → absent.
got=$("$MB" -c 'NOT_EXPORTED=hidden; sh -c "echo \${NOT_EXPORTED:-absent}"')
check "unexported variable not visible in child" "$got" "absent"

# --- readonly: assignment rejected ---
"$MB" -c 'readonly RO=fixed; RO=changed' 2>/dev/null
# Assigning to a read-only variable is a variable assignment error: the
# non-interactive shell exits there and then (POSIX 2.8.1, status 2 as in dash),
# rather than reporting 1 and running the rest of the script.
check_exit "readonly: reassignment is fatal" "$?" "2"
got=$("$MB" -c 'readonly RO=fixed; RO=changed; echo SHOULD_NOT_PRINT' 2>/dev/null)
check "readonly: reassignment stops the script" "$got" ""

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
# `&` already terminates the command: `&;` is a syntax error in dash and bash,
# and this test only ever passed because silex used to accept the stray `;`.
got=$("$MB" -c 'sleep 0 & BGPID=$!; wait; echo $BGPID')
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

# --- F-03: IFS edge cases ---

# IFS= read: preserves leading/trailing whitespace
got=$("$MB" -c 'printf "  hello world  \n" | { IFS= read line; echo "$line"; }')
check "IFS= read: preserves leading whitespace" "$got" "  hello world  "

# IFS='' read: same as IFS= (no splitting, preserves whitespace)
got=$("$MB" -c 'printf "  hello world  \n" | { IFS="" read x; echo "$x"; }')
check 'IFS="" read: preserves leading whitespace' "$got" "  hello world  "

# unset IFS: default splitting on whitespace
got=$("$MB" -c 'unset IFS; V="a  b  c"; for w in $V; do printf "%s\n" "$w"; done')
check "unset IFS: default whitespace splitting" "$got" "$(printf 'a\nb\nc')"

# IFS=: custom separator in for loop
got=$("$MB" -c 'IFS=:; V="x:y:z"; for w in $V; do printf "%s\n" "$w"; done')
check "IFS=: splits on colon in for loop" "$got" "$(printf 'x\ny\nz')"

# --- special parameters ($#, $?, $$...) inside arithmetic --------------------
# The arith parser read only name characters after '$', so $# / $? / $$ became
# an empty name -- harmless normally (0) but "unbound variable" under set -u,
# which broke modernish FTL_HASHVAR.
check "arith: \$# is the positional count" \
    "$("$MB" -c 'set -- a b c; echo "$(($# - 1))"')" "2"
check "arith: \$# under set -u does not error" \
    "$("$MB" -c 'set -u; set -- x y; echo "$(($#))"')" "2"
check "arith: \$? under set -u" \
    "$("$MB" -c 'set -u; true; echo "$(($? + 5))"')" "5"
check "arith: \$\$ is usable in arithmetic" \
    "$("$MB" -c 'set -u; echo "$(($$ - $$))"')" "0"

# --- `sh -c cmd name arg...`: first operand after the command string is $0 -----
# POSIX. silex used to set $0 to its own binary and shift every operand by one,
# which broke `sh -c '. "$1"' shell std fatal` (it tried to source the shell).
check "dashc: first operand is \$0, rest are \$1.." \
    "$("$MB" -c 'echo "$0 $1 $2 n=$#"' NAME a1 a2)" "NAME a1 a2 n=2"
check "dashc: \$# counts only args after command_name" \
    "$("$MB" -c 'echo "$#"' myname x y z)" "3"
check "dashc: no positionals when only command_name given" \
    "$("$MB" -c 'echo "n=$# 1=[$1]"' just0)" "n=0 1=[]"

# --- $$ is the main shell PID, constant across subshells (POSIX) ---------------
# It was re-read with getpid() each time, so a fork for $(...), a pipeline, or
# ( ) changed it -- breaking `case $(...) in ($$)`, which is how modernish
# identifies a usable shell.
check "dollardollar: constant inside command substitution" \
    "$("$MB" -c 'a=$$; b=$(echo $$); [ "$a" = "$b" ] && echo SAME || echo DIFF')" "SAME"
check "dollardollar: constant inside ( ) subshell" \
    "$("$MB" -c 'a=$$; ( [ "$a" = "$$" ] && echo SAME || echo DIFF )')" "SAME"
check "dollardollar: case \$(echo \$\$) matches \$\$ pattern" \
    "$("$MB" -c 'case $(echo $$) in ( $$ ) echo MATCH;; *) echo NO;; esac')" "MATCH"

# --- command -v on a pathname reports it verbatim if executable (POSIX) --------
check "commandv: absolute path to executable is echoed" \
    "$("$MB" -c 'command -v /bin/sh')" "/bin/sh"
"$MB" -c 'command -v /nonexistent/xyz' >/dev/null 2>&1
check_exit "commandv: nonexistent pathname returns non-zero" "$?" "127"

# -----------------------------------------------------------------------
# A simple command whose words expand to NOTHING (POSIX 2.9.1)
# -----------------------------------------------------------------------
# There is no command name, but the command still performs its redirections
# and its assignments, and completes with the status of the last command
# substitution. silex used to treat it as a no-op returning 0, losing all
# three.
check "empty argv: \$(false) is the command's status" \
    "$("$MB" -c '$(false); echo $?')" "1"
check "empty argv: \$(exit 42) is the command's status" \
    "$("$MB" -c '$(exit 42); echo $?')" "42"
check "empty argv: the LAST substitution wins" \
    "$("$MB" -c '$(exit 42) $(exit 43); echo $?')" "43"
check "empty argv: backtick form too" \
    "$("$MB" -c '`false`; echo $?')" "1"
check "empty argv: a real command name is unaffected" \
    "$("$MB" -c 'true $(false); echo $?')" "0"
check "empty argv: no substitution at all is 0" \
    "$("$MB" -c 'unset u; $u; echo $?')" "0"
check "empty argv: status is visible to 'if'" \
    "$("$MB" -c 'if `false`; then echo T; else echo F; fi')" "F"
# POSIX expands the words (step 2) before the assignments (step 4), so an
# assignment's substitution is the LAST one performed and outranks a word's.
check "empty argv: an assignment's substitution outranks a word's" \
    "$("$MB" -c 'foo=$(exit 1) $(exit 2); echo $?')" "1"
# With no command name the binding is not a temp binding -- it persists,
# exactly as if the words had not been written at all (Oils temp-binding 3).
check "empty argv: the assignment persists" \
    "$("$MB" -c 'foo=alive! $unset; echo $foo')" "alive!"
EMPTYARGV_D=$(mktemp -d)
check "empty argv: redirections are still performed" \
    "$("$MB" -c 'unset u; $u > '"$EMPTYARGV_D"'/f; test -f '"$EMPTYARGV_D"'/f && echo made')" "made"
rm -rf "$EMPTYARGV_D"
check "empty argv: a failed redirection skips the assignment" \
    "$("$MB" -c 'unset u; v=set $u < /nonexistent; echo "[$v]"' 2>/dev/null)" "[]"
check "empty argv: set -e acts on the substitution's status" \
    "$("$MB" -c 'set -e; $(false); echo reached')" ""


# --- memory stays flat under repeated redefinition ---------------------------
# Run BODY 20000 times in ONE silex shell after a 2000-run warm-up and print
# "flat" when VmRSS grew by less than 256 KB over the measured runs. Every
# BODY below used to copy something into the shell's never-freed parse arena
# (or leak it some other way) on each run: tens of bytes to kilobytes per
# iteration, i.e. megabytes here, and an abort at the arena's 64 MB cap in a
# long enough loop.
# Not judged under a sanitised build: ASan's allocator and quarantine grow on
# their own, so VmRSS says nothing there (same rule as tests/leak).
rss_sanitised() {
    { command -v nm >/dev/null 2>&1 && nm "$MB" 2>/dev/null | grep __asan_init >/dev/null; } ||
    { command -v ldd >/dev/null 2>&1 && ldd "$MB" 2>/dev/null | grep libasan >/dev/null; }
}
rss_flat() {
    if [ ! -r /proc/self/status ] || rss_sanitised; then echo flat; return; fi
    "$MB" -c '
        rss() { while read -r k v u; do [ "$k" = VmRSS: ] && R=$v; done </proc/$$/status; }
        i=0; while [ $i -lt 2000 ]; do eval "$1"; i=$((i+1)); done
        rss; r1=$R
        i=0; while [ $i -lt 20000 ]; do eval "$1"; i=$((i+1)); done
        rss; g=$((R - r1))
        if [ "$g" -lt 256 ]; then echo flat; else echo "grew ${g} KB"; fi
    ' rss_flat "$1"
}
LONG="padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding-padding"

check "reassigning an exported variable in a loop: memory stays flat" \
    "$(rss_flat 'export EV=/usr/bin:$i:$LONG')" "flat"
check "assigning an already-exported variable in a loop: memory stays flat" \
    "$(rss_flat 'PATH_X=/usr/bin:$i:$LONG; export PATH_X; PATH_X=$PATH_X:x')" "flat"
check "export + unset in a loop: memory stays flat" \
    "$(rss_flat 'export EU=$i$LONG; unset EU')" "flat"
check "VAR=value prefix on an applet in a loop: memory stays flat" \
    "$(rss_flat 'EP=$i$LONG basename a >/dev/null')" "flat"
check "global assign + unset in a loop: memory stays flat" \
    "$(rss_flat 'GV=$i$LONG; unset GV')" "flat"

# ...while the environment still always carries the CURRENT value, for
# children and for in-process applets that read it with getenv().
check "child sees each new value of an exported variable" \
    "$("$MB" -c 'export EV=one; sh -c "echo \$EV"; EV=two; sh -c "echo \$EV"; EV=three; env | grep "^EV="' | tr '\n' ' ')" \
    "one two EV=three "
check "unset removes an exported variable from the environment" \
    "$("$MB" -c 'export EV=one; EV=two; unset EV; env | grep -c "^EV="')" "0"
check "re-export after unset carries the new value" \
    "$("$MB" -c 'export EV=one; unset EV; EV=two; export EV; env | grep "^EV="')" "EV=two"
TD1=$(mktemp -d); TD2=$(mktemp -d)
got=$("$MB" -c 'export TMPDIR="$1"; a=$(mktemp); TMPDIR="$2"; b=$(mktemp); case $a in "$1"/*) echo a-ok;; esac; case $b in "$2"/*) echo b-ok;; esac' sh "$TD1" "$TD2" | tr '\n' ' ')
check "in-process mktemp follows each new TMPDIR" "$got" "a-ok b-ok "
rm -rf "$TD1" "$TD2"
check "in-process date sees the current TZ after reassignment" \
    "$("$MB" -c 'export TZ=UTC; date -d @0 +%H; TZ=UTC+5; date -d @0 +%H')" "$(printf '00\n19')"
check "exported variable inherited from the environment can be reassigned" \
    "$(EVI=orig "$MB" -c 'EVI=new; sh -c "echo \$EVI"; unset EVI; sh -c "echo \${EVI-gone}"' | tr '\n' ' ')" "new gone "
check "a value assigned from itself survives a growing reassignment" \
    "$("$MB" -c 'x=ab; x=$x$x$x$x$x$x$x$x; x=${x#a}; echo $x; y=12; y=$y$y$y$y; y=$y; echo $y')" \
    "$(printf 'bababababababab\n12121212')"

echo ""
echo "variable tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
