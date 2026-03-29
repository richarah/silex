#!/bin/bash
# test_shell_builtins.sh — shell builtin integration tests
# Covers: applet short-circuit regression, umask, command, type, getopts
# Usage: test_shell_builtins.sh [path/to/matchbox]

MATCHBOX="${1:-build/bin/matchbox}"
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
    "$("$MATCHBOX" sh -c 'printf "a\nb\nc\n" | wc -l' | tr -d ' ')"

check "applet: sort via shell" \
    "$(printf 'a\nb\nc')" \
    "$("$MATCHBOX" sh -c 'printf "c\na\nb\n" | sort')"

check "applet: grep via shell" \
    "bb" \
    "$("$MATCHBOX" sh -c 'printf "aa\nbb\ncc\n" | grep bb')"

check "applet: sed via shell" \
    "world" \
    "$("$MATCHBOX" sh -c 'echo hello | sed s/hello/world/')"

check "applet: tr via shell" \
    "HELLO" \
    "$("$MATCHBOX" sh -c 'echo hello | tr a-z A-Z')"

check "applet: cut via shell" \
    "b" \
    "$("$MATCHBOX" sh -c 'echo a:b:c | cut -d: -f2')"

check "applet: basename via shell" \
    "bin" \
    "$("$MATCHBOX" sh -c 'basename /usr/local/bin')"

check "applet: dirname via shell" \
    "/usr/local" \
    "$("$MATCHBOX" sh -c 'dirname /usr/local/bin')"

# umask
check "umask: set and print" \
    "0022" \
    "$("$MATCHBOX" sh -c 'umask 022; umask')"

check "umask: -S flag has =" \
    "1" \
    "$("$MATCHBOX" sh -c 'umask -S' | grep -c '=')"

check "umask: print is 4 octal digits" \
    "1" \
    "$("$MATCHBOX" sh -c 'umask 0177; umask' | grep -c '^[0-7][0-7][0-7][0-7]$')"

# command -v
check "command -v builtin" \
    "echo" \
    "$("$MATCHBOX" sh -c 'command -v echo')"

check "command -v external finds path" \
    "1" \
    "$("$MATCHBOX" sh -c 'command -v ls' | grep -c '/')"

check "command -v notfound exits 1" \
    "1" \
    "$("$MATCHBOX" sh -c 'command -v _doesnotexist_xyz_ >/dev/null 2>&1; echo $?')"

# type
check "type builtin" \
    "1" \
    "$("$MATCHBOX" sh -c 'type echo' | grep -c builtin)"

check "type external finds path" \
    "1" \
    "$("$MATCHBOX" sh -c 'type ls' | grep -c '/')"

# getopts
check "getopts: basic single flag" \
    "a" \
    "$("$MATCHBOX" sh -c 'getopts ab opt -a; echo $opt')"

check "getopts: option with argument" \
    "x" \
    "$("$MATCHBOX" sh -c 'getopts a: opt -a x; echo $OPTARG')"

check "getopts: loop over multiple flags" \
    "abc" \
    "$("$MATCHBOX" sh -c 'OPTIND=1; while getopts abc opt -a -b -c; do printf "%s" "$opt"; done; echo')"

echo ""
echo "shell_builtins: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
