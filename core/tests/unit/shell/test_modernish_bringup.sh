#!/bin/sh
# tests/unit/shell/test_modernish_bringup.sh
#
# Regression tests for the batch of POSIX-conformance fixes that took modernish
# from "crashes during init" to "runs its regression test suite". Each block
# names the construct in modernish that exercised the bug.
#
# Usage: ./test_modernish_bringup.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
case "$SILEX" in /*) ;; *) SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;; esac
MB="$SILEX"
PASS=0
FAIL=0

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT INT TERM

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

# -----------------------------------------------------------------------
# Lexer: escaped '(' in a case pattern inside $(...) (safe.mm)
# -----------------------------------------------------------------------
check "escaped \\( case pattern inside a double-quoted cmdsub parses" \
    "$("$MB" -c 'g() { x="$(case $a in ( \( ) echo p ;; esac)"; echo ok; }; g')" "ok"

# -----------------------------------------------------------------------
# Lexer: comment inside $(...) must not let a stray ' " ) derail the scan
# -----------------------------------------------------------------------
printf 'x=$(\n# a comment with an apostrophe: it'"'"'s and a ) paren\necho hi\n)\necho "[$x]"\n' > "$TMPD/cm.sh"
check "apostrophe/paren in a cmdsub comment do not break the scan" \
    "$("$MB" "$TMPD/cm.sh" 2>&1)" "[hi]"

# -----------------------------------------------------------------------
# Lexer: $'...' is POSIX literal-$ + quoted string, not a runaway scan (CESCQUOT)
# -----------------------------------------------------------------------
check "\$'...' in a case subject does not swallow 'in'" \
    "$("$MB" -c "case \$'a\\40b' in ( 'a b' ) echo m ;; ( * ) echo nomatch ;; esac")" "nomatch"
check "\$'...' in a for list does not swallow 'do'" \
    "$("$MB" -c "for x in \$'a' b; do printf '%s ' \"\$x\"; done")" '$a b '

# -----------------------------------------------------------------------
# Parser: reserved words are ordinary data in a for word list (builtin.t)
# -----------------------------------------------------------------------
check "reserved words are valid for-list values" \
    "$("$MB" -c 'for v in ! { } case do done fi for if then while; do printf "%s " "$v"; done')" \
    '! { } case do done fi for if then while '

# -----------------------------------------------------------------------
# Shell: POSIX default variables under set -u (OPTIND, PS4)
# -----------------------------------------------------------------------
check "OPTIND defaults to 1 under set -u" \
    "$("$MB" -c 'set -u; echo "$OPTIND"')" "1"
check "PS4 has a default under set -u" \
    "$("$MB" -c 'set -u; echo "[$PS4]"')" "[+ ]"

# -----------------------------------------------------------------------
# set builtin: `set --` clears positionals; `set -e` leaves them (DOTARG)
# -----------------------------------------------------------------------
check "set -- clears the positional parameters" \
    "$("$MB" -c 'set -- a b c; set --; echo "$#"')" "0"
check "set -e (options only) leaves the positionals untouched" \
    "$("$MB" -c 'set -- a b c; set -e; echo "$#"')" "3"

# A file's `set --` is visible to a script it then sources (nested dot).
printf 'echo "$#"\n' > "$TMPD/inner.sh"
printf 'set --\n. "%s/inner.sh"\n' "$TMPD" > "$TMPD/outer.sh"
check "set -- in a sourced file reaches a nested source" \
    "$("$MB" -c "set -- X Y; . '$TMPD/outer.sh'")" "0"

# -----------------------------------------------------------------------
# set -o / +o listing, and option round-trip (var/stack, _IN/opt)
# -----------------------------------------------------------------------
check "set +o lists reusable option commands" \
    "$("$MB" -c 'set -C; set +o' | grep noclobber)" "set -o noclobber"
check "options survive a set +o save / eval restore round-trip" \
    "$("$MB" -c 'set -C; s=$(set +o); set +C; eval "$s"; case $- in *C*) echo yes;; *) echo no;; esac')" \
    "yes"

# -----------------------------------------------------------------------
# noclobber: `>` refuses to overwrite; `>|` forces; command aborts on failure
# -----------------------------------------------------------------------
echo orig > "$TMPD/nc"
"$MB" -c "set -C; echo new > '$TMPD/nc'" 2>/dev/null
check "set -C: > does not overwrite an existing file" "$(cat "$TMPD/nc")" "orig"
"$MB" -c "set -C; echo forced >| '$TMPD/nc'" 2>/dev/null
check "set -C: >| forces the overwrite" "$(cat "$TMPD/nc")" "forced"

# -----------------------------------------------------------------------
# A failed redirection aborts the command and reports non-zero
# -----------------------------------------------------------------------
check "a failed redirection skips the command (non-zero, no run)" \
    "$("$MB" -c 'echo NOPE > /no_such_dir_xyz/f 2>/dev/null; echo "rc=$?"')" "rc=1"

# command-not-found honours the command's redirections
check "command-not-found message obeys 2>/dev/null" \
    "$("$MB" -c 'no_such_cmd_xyz 2>/dev/null; echo "rc=$?"')" "rc=127"

# -----------------------------------------------------------------------
# allexport: set -a auto-exports; set +a stops; $- reflects it (bin/modernish)
# -----------------------------------------------------------------------
check "set -a auto-exports assigned variables" \
    "$("$MB" -c 'set -a; FOO=bar; sh -c "printf %s \"\$FOO\""')" "bar"
check "set +a stops auto-export" \
    "$("$MB" -c 'set -a; set +a; BAZ=qux; sh -c "printf [%s] \"\$BAZ\""')" "[]"

# -----------------------------------------------------------------------
# Pathname expansion applies to the value of an unquoted expansion (LOCAL --fglob)
# -----------------------------------------------------------------------
: > "$TMPD/g1.tt"; : > "$TMPD/g2.tt"
check "a glob pattern from a variable value is expanded" \
    "$(cd "$TMPD" && "$MB" -c 'p="*.tt"; set -- $p; echo "$#"')" "2"
check "a quoted expansion is NOT globbed" \
    "$(cd "$TMPD" && "$MB" -c 'p="*.tt"; echo "$p"')" '*.tt'

# -----------------------------------------------------------------------
# `command exec REDIR` persists the redirection to the current shell (loop.mm)
# -----------------------------------------------------------------------
printf 'file-data\n' > "$TMPD/fd"
check "command exec 8<file opens the fd in the current shell" \
    "$("$MB" -c "command exec 8< '$TMPD/fd'; IFS= read -r L <&8; echo \"\$L\"")" "file-data"

# -----------------------------------------------------------------------
# A PATH= prefix is used to resolve the command, even via `command` (str ematch)
# -----------------------------------------------------------------------
mkdir -p "$TMPD/bin"
printf '#!/bin/sh\necho found-via-prefix\n' > "$TMPD/bin/mytool"
chmod +x "$TMPD/bin/mytool"
check "PATH= prefix resolves the command" \
    "$("$MB" -c "PATH=/nowhere; PATH='$TMPD/bin' mytool")" "found-via-prefix"
check "PATH= prefix resolves through 'command'" \
    "$("$MB" -c "PATH=/nowhere; PATH='$TMPD/bin' command mytool")" "found-via-prefix"

# -----------------------------------------------------------------------
# Command substitution inherits the parent's shell options (set +o via _IN/opt)
# -----------------------------------------------------------------------
check "a command substitution inherits set -f" \
    "$(cd "$TMPD" && "$MB" -c 'set -f; echo "$(echo *.tt)"')" '*.tt'

# -----------------------------------------------------------------------
# Arithmetic: ${n-default} inside $(( )) does full parameter expansion (inc/dec)
# -----------------------------------------------------------------------
check "arith \${2-1} defaults, not looked up as a variable named 2-1" \
    "$("$MB" -c 'set -u; echo $(( ${2-1} ))')" "1"
check "modernish inc pattern: num += (\${2-1})" \
    "$("$MB" -c 'set -u; num=0; : "$(( num += (${2-1}) ))"; echo "$num"')" "1"

# -----------------------------------------------------------------------
# Memory: a function-calling loop stays flat (per-call scope arenas reclaimed)
# and repeated eval does not grow the parse arena (node_dup out of transients).
# Before these fixes both hit the 64 MB arena cap and aborted.
# -----------------------------------------------------------------------
check "200k function calls with a local do not exhaust the arena" \
    "$("$MB" -c 'f() { local x=$1; y=$((x+1)); }; i=0; while [ $i -lt 200000 ]; do f $i; i=$((i+1)); done; echo "$y"')" \
    "200000"
check "60k eval iterations do not exhaust the parse arena" \
    "$("$MB" -c 'i=0; while [ $i -lt 60000 ]; do eval "z=$i"; i=$((i+1)); done; echo "$z"')" \
    "59999"

# -----------------------------------------------------------------------
# Declared-but-unset variables: `export NAME` / `readonly NAME` on an unset
# name record the attribute WITHOUT setting the variable, so it stays unset
# (distinct from empty). modernish's `isset -x` / `isset -r` on an unset name
# rely on this and on `export -p` / `readonly -p` listing the bare name.
# -----------------------------------------------------------------------
check "export NAME on an unset var keeps it unset (\${x+SET} empty)" \
    "$("$MB" -c 'unset foo; export foo; echo "[${foo+SET}]"')" "[]"
check "export NAME on an unset var: \${x-DEF} defaults" \
    "$("$MB" -c 'unset foo; export foo; echo "[${foo-DEF}]"')" "[DEF]"
check "export -p lists a declared-but-unset exported var as a bare name" \
    "$("$MB" -c 'unset foo; export foo; export -p | grep "foo"')" "export foo"
check "readonly NAME on an unset var keeps it unset (\${x+SET} empty)" \
    "$("$MB" -c 'unset bar; readonly bar; echo "[${bar+SET}]"')" "[]"
check "readonly -p lists a declared-but-unset readonly var as a bare name" \
    "$("$MB" -c 'unset bar; readonly bar; readonly -p | grep bar')" "readonly bar"
check "a declared-but-unset exported var is not in the environment yet" \
    "$("$MB" -c 'unset onlydecl; export onlydecl; env | grep "^onlydecl=" || echo none')" "none"
check "assigning a declared-but-unset exported var enters the environment" \
    "$("$MB" -c 'unset baz; export baz; baz=hi; env | grep "^baz="')" "baz=hi"
check "set (no args) does not list a declared-but-unset var" \
    "$("$MB" -c 'unset onlydecl; export onlydecl; set | grep "^onlydecl" || echo absent')" "absent"
check "a readonly declared-but-unset var still cannot be assigned" \
    "$("$MB" -c 'unset q; readonly q; q=val; echo "[${q-UNSET}]"' 2>/dev/null)" "[UNSET]"

# -----------------------------------------------------------------------
# ulimit builtin: modernish probes it with `thisshellhas --bi=ulimit` (a POSIX
# regular builtin that must be built in to alter the shell's own limits) and
# uses it to force a subshell fork. It must resolve as a builtin (found even
# with PATH=/dev/null) and get/set resource limits like dash.
# -----------------------------------------------------------------------
check "ulimit resolves as a builtin (PATH=/dev/null command -v)" \
    "$("$MB" -c 'PATH=/dev/null command -v ulimit')" "ulimit"
check "ulimit -n prints a soft file-descriptor limit" \
    "$("$MB" -c 'case $(ulimit -n) in ([0-9]*|unlimited) echo ok;; (*) echo no;; esac')" "ok"
check "ulimit -t set in a subshell reads back the same value" \
    "$("$MB" -c '(ulimit -t 3600; ulimit -t)')" "3600"
check "ulimit -f (default resource) matches an explicit -f" \
    "$("$MB" -c 'test "$(ulimit)" = "$(ulimit -f)" && echo same')" "same"
check "ulimit rejects an unknown option with status 2" \
    "$("$MB" -c 'ulimit -Z 2>/dev/null; echo $?')" "2"

# -----------------------------------------------------------------------
# getopts: on a missing required argument or an unknown option, POSIX unsets
# OPTARG in errmsg mode (optstring not starting with ':') and sets it to the
# option character only in silent mode. silex used to set OPTARG in both modes,
# so modernish's "getopts val for no opt-arg (errmsg mode)" saw the option
# letter where dash/bash leave OPTARG unset.
# -----------------------------------------------------------------------
check "getopts missing arg, errmsg mode: name=? and OPTARG unset" \
    "$("$MB" -c 'OPTIND=1; set -- -x; getopts x: o 2>/dev/null; echo "$o,${OPTARG-UNSET}"')" "?,UNSET"
check "getopts missing arg, silent mode: name=: and OPTARG is the option char" \
    "$("$MB" -c 'OPTIND=1; set -- -x; getopts :x: o 2>/dev/null; echo "$o,${OPTARG-UNSET}"')" ":,x"
check "getopts unknown option, errmsg mode: name=? and OPTARG unset" \
    "$("$MB" -c 'OPTIND=1; set -- -q; getopts x: o 2>/dev/null; echo "$o,${OPTARG-UNSET}"')" "?,UNSET"
check "getopts unknown option, silent mode: OPTARG is the option char" \
    "$("$MB" -c 'OPTIND=1; set -- -q; getopts :x: o 2>/dev/null; echo "$o,${OPTARG-UNSET}"')" "?,q"
check "modernish getopts errmsg loop yields ?,Empty for the trailing missing arg" \
    "$("$MB" -c 'OPTIND=1 v=; set -- -xfoo -yz; while getopts x:yz: opt >/dev/null 2>&1; do v="${v:+$v/}$opt,${OPTARG:-Empty}"; done; echo "$v"')" \
    "x,foo/y,Empty/?,Empty"

# -----------------------------------------------------------------------
echo
echo "modernish-bringup tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
