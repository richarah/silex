#!/bin/bash
# test_shell_builtins.sh — shell builtin integration tests
# Covers: applet short-circuit regression, umask, command, type, getopts
# Usage: test_shell_builtins.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
PASS=0; FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS+1))
    else
        echo "  FAIL: $desc (expected='$expected' got='$actual')"
        FAIL=$((FAIL+1))
    fi
}

# Applet short-circuit regression: tools beyond original 3 must work from shell
# If the old 3-entry stub were reintroduced, these would fork-exec externals or fail
check "applet: wc via shell" \
    "3" \
    "$("$SILEX" sh -c 'printf "a\nb\nc\n" | wc -l' | tr -d ' ')"

check "applet: sort via shell" \
    "$(printf 'a\nb\nc')" \
    "$("$SILEX" sh -c 'printf "c\na\nb\n" | sort')"

check "applet: grep via shell" \
    "bb" \
    "$("$SILEX" sh -c 'printf "aa\nbb\ncc\n" | grep bb')"

check "applet: sed via shell" \
    "world" \
    "$("$SILEX" sh -c 'echo hello | sed s/hello/world/')"

check "applet: tr via shell" \
    "HELLO" \
    "$("$SILEX" sh -c 'echo hello | tr a-z A-Z')"

check "applet: cut via shell" \
    "b" \
    "$("$SILEX" sh -c 'echo a:b:c | cut -d: -f2')"

check "applet: basename via shell" \
    "bin" \
    "$("$SILEX" sh -c 'basename /usr/local/bin')"

check "applet: dirname via shell" \
    "/usr/local" \
    "$("$SILEX" sh -c 'dirname /usr/local/bin')"

# umask
check "umask: set and print" \
    "0022" \
    "$("$SILEX" sh -c 'umask 022; umask')"

check "umask: -S flag has =" \
    "1" \
    "$("$SILEX" sh -c 'umask -S' | grep -c '=')"

check "umask: print is 4 octal digits" \
    "1" \
    "$("$SILEX" sh -c 'umask 0177; umask' | grep -c '^[0-7][0-7][0-7][0-7]$')"

# command -v
check "command -v builtin" \
    "echo" \
    "$("$SILEX" sh -c 'command -v echo')"

check "command -v external finds path" \
    "1" \
    "$("$SILEX" sh -c 'command -v ls' | grep -c '/')"

check "command -v notfound exits 1" \
    "1" \
    "$("$SILEX" sh -c 'command -v _doesnotexist_xyz_ >/dev/null 2>&1; echo $?')"

# type
check "type builtin" \
    "1" \
    "$("$SILEX" sh -c 'type echo' | grep -c builtin)"

check "type external finds path" \
    "1" \
    "$("$SILEX" sh -c 'type ls' | grep -c '/')"

# getopts
check "getopts: basic single flag" \
    "a" \
    "$("$SILEX" sh -c 'getopts ab opt -a; echo $opt')"

check "getopts: option with argument" \
    "x" \
    "$("$SILEX" sh -c 'getopts a: opt -a x; echo $OPTARG')"

check "getopts: loop over multiple flags" \
    "abc" \
    "$("$SILEX" sh -c 'OPTIND=1; while getopts abc opt -a -b -c; do printf "%s" "$opt"; done; echo')"

# --- ulimit rejects what it used to silently accept ---
# Extra operands were dropped and a leading '-' was fed to strtoull, which
# wraps it: `ulimit -f -- -42` reported success after setting a limit of
# 2^64-42 blocks. A mistyped limit must fail loudly, not look applied.
check "ulimit: extra operand is an error" \
    "2" \
    "$("$SILEX" sh -c 'ulimit 1 2 2>/dev/null; echo $?')"
check "ulimit: -a takes no operand" \
    "2" \
    "$("$SILEX" sh -c 'ulimit -a 42 2>/dev/null >/dev/null; echo $?')"
check "ulimit: negative limit is a bad number" \
    "2" \
    "$("$SILEX" sh -c 'ulimit -f -- -42 2>/dev/null; echo $?')"
check "ulimit: a plain query still works" \
    "0" \
    "$("$SILEX" sh -c 'ulimit -f >/dev/null; echo $?')"

# --- trap knows the rest of the POSIX signal names ---
# `trap - XFSZ` is what a script writes before deliberately exceeding
# `ulimit -f`; it used to fail with "bad trap".
check "trap: XFSZ is a known signal" \
    "ok" \
    "$("$SILEX" sh -c 'trap - XFSZ 2>/dev/null && echo ok')"
check "trap: XCPU is a known signal" \
    "ok" \
    "$("$SILEX" sh -c 'trap - XCPU 2>/dev/null && echo ok')"

# --- command -p searches the DEFAULT path, for execution and not just -v ---
# -p was parsed and then ignored on the execution path, so it happily ran a
# utility the caller had planted on $PATH -- the one thing -p exists to prevent.
check "command -p ignores \$PATH for lookup" \
    "127" \
    "$("$SILEX" sh -c 'd=$(mktemp -d); printf "#!/bin/sh\necho planted\n" > $d/plantedcmd; chmod +x $d/plantedcmd; PATH=$d; command -p plantedcmd >/dev/null 2>&1; echo $?')"
check "command -p finds a standard utility with \$PATH empty" \
    "0" \
    "$("$SILEX" sh -c 'PATH=""; command -p ls >/dev/null 2>&1; echo $?')"

# --- date operands ---
# A bare operand is the obsolescent set-the-clock form, NOT a format string:
# `date %x` printed the time and succeeded where every other date rejects it,
# so a configure probe testing `date FMT` took the wrong branch.
check "date: a non-+ operand is an invalid date, not a format" \
    "1" \
    "$("$SILEX" sh -c 'date %x >/dev/null 2>&1; echo $?')"
check "date: +FORMAT still works" \
    "ok" \
    "$("$SILEX" sh -c 'date +ok')"
check "date: a second operand is an extra operand" \
    "1" \
    "$("$SILEX" sh -c 'date +%Y +%m >/dev/null 2>&1; echo $?')"

# --- test/[ expression grammar: the three rules dash draws and silex did not ---
# 1. A primary that runs out of words is a MISSING EXPRESSION (false), not a
#    syntax error. This is what makes the `-a -a -a ...` ladder (Oils blog2)
#    return a status: every `-a` is a non-empty string (true) and the dangling
#    one on the right takes the AND to false.
st() { "$SILEX" sh -c "$1"' >/dev/null 2>&1; echo $?'; }
check "test: [ -a -a ] is false, not an error"            "1" "$(st '[ -a -a ]')"
check "test: [ -a -a -a ] is true"                        "0" "$(st '[ -a -a -a ]')"
check "test: [ -a -a -a -a ] is false"                    "1" "$(st '[ -a -a -a -a ]')"
check "test: [ -a -a -a -a -a ] is true"                  "0" "$(st '[ -a -a -a -a -a ]')"
check "test: [ -a -a -a -a -a -a ] is false"              "1" "$(st '[ -a -a -a -a -a -a ]')"
check "test: trailing -a operand missing is false"        "1" "$(st '[ a -a ]')"
check "test: trailing -o operand missing short-circuits"  "0" "$(st '[ a -o ]')"
check "test: ! with no operand is true"                   "0" "$(st '[ x -a ! ]')"

# 2. A BINARY operator wins over a unary one, but only when its right operand
#    exists. `test $# -ne 0 -a "$1" != "--"` (the sqsh configure idiom behind
#    Oils #2409) hinges on "-z" being the LEFT OPERAND of `!=`, not a string
#    test whose operand is "!=" with a stray trailing "--".
check "test: -z is the left operand of != when one follows" \
    "0" "$(st 'set -- -z; test $# -ne 0 -a "$1" != "--"')"
check "test: -f is the left operand of = when one follows" \
    "1" "$(st 'test x -a -f = y')"
check "test: -e reverts to a file test when = has no operand" \
    "1" "$(st '[ x -a -e = ]')"
check "test: a binary operator with no right operand is an error" \
    "2" "$(st '[ x -a y = ]')"

# 3. `-t` takes a file descriptor NUMBER; garbage is status 2, not false.
check "test: -t with a non-numeric operand is an error" "2" "$(st '[ -t invalid ]')"
check "test: -t inside an expression is an error too"   "2" "$(st '[ x -a -t invalid ]')"
check "test: -t under ! is still an error"              "2" "$(st '[ ! -t invalid ]')"

# --- the `env` applet must not take the shell with it ---
# env EXECS the command it is given. That is right for a standalone
# /usr/bin/env and fatal for an applet dispatched in-process: the shell was
# simply replaced, so everything after `env CMD` silently never ran and the
# script still exited 0.
check "env: the script continues after 'env CMD'" \
    "before mid after" \
    "$("$SILEX" sh -c 'echo before; env echo mid; echo after' | tr '\n' ' ' | sed 's/ $//')"
check "env: exit status of the command is reported" \
    "1" \
    "$("$SILEX" sh -c 'env false; echo $?')"
check "env: a missing command is 127" \
    "127" \
    "$("$SILEX" sh -c 'env nosuchcmd_xyz 2>/dev/null; echo $?')"
check "env: NAME=VAL operands reach the command" \
    "1-2" \
    "$("$SILEX" sh -c 'FOO=1 env BAR=2 sh -c "echo \$FOO-\$BAR"')"

# --- a VAR=val prefix must reach an APPLET's environment ---
# An external utility gets the binding from the exec; an applet runs
# in-process and reads getenv(), so `TZ=UTC date` printed local time and
# `LC_ALL=C sort` sorted in the caller's locale. Applets beat PATH by design,
# so these idioms were broken for every script.
check "applet env: TZ=UTC reaches the date applet" \
    "$(TZ=UTC date +%H)" \
    "$("$SILEX" sh -c 'TZ=UTC date +%H')"
check "applet env: TZ=Asia/Tokyo reaches the date applet" \
    "$(TZ=Asia/Tokyo date +%H)" \
    "$("$SILEX" sh -c 'TZ=Asia/Tokyo date +%H')"
check "applet env: the binding does not outlive the command" \
    "TZ=[] inenv=0" \
    "$("$SILEX" sh -c 'TZ=UTC date +%H >/dev/null; echo "TZ=[$TZ] inenv=$(env | grep -c "^TZ=")"')"
check "applet env: an unexported var is not left exported" \
    "V=[orig] inenv=0" \
    "$("$SILEX" sh -c 'V=orig; V=temp date +%H >/dev/null; echo "V=[$V] inenv=$(env | grep -c "^V=")"')"
check "applet env: an exported var keeps its old value and export" \
    "V=[orig] inenv=1" \
    "$("$SILEX" sh -c 'export V=orig; V=temp date +%H >/dev/null; echo "V=[$V] inenv=$(env | grep -c "^V=orig")"')"

echo ""
echo "shell_builtins: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
