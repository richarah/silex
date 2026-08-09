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
# unset must remove a variable from the process environment, not just the
# shell table. Otherwise `export V=x; unset V` leaves V=x visible to children,
# which made modernish falsely detect BUG_EXPORTUNS (it reuses _Msh_test as a
# scratch var: exported with a value, then unset).
# -----------------------------------------------------------------------
check "unset removes an exported var from a child's environment" \
    "$("$MB" -c 'export V=leftover; unset V; '"$MB"' -c '\''echo "${V+SET}${V-NO}"'\''')" \
    "NO"
check "export of a still-unset name after unset does not leak an empty var" \
    "$("$MB" -c 'export _Msh_test=x; unset -v _Msh_test; export _Msh_test; '"$MB"' -c '\''echo "[${_Msh_test+SET}]"'\''')" \
    "[]"

# -----------------------------------------------------------------------
# `test -o OPTION` (ksh/bash extension) reports shell option state; modernish
# detects it as TESTO and uses it for `isset -o`. It used to fall through to
# false even when the option was on.
# -----------------------------------------------------------------------
check "test -o nounset is true under set -u" \
    "$("$MB" -c 'set -u; test -o nounset && echo on || echo off')" "on"
check "test -o nounset is false without set -u" \
    "$("$MB" -c 'test -o nounset && echo on || echo off')" "off"
check "binary -o (a -o b) still works" \
    "$("$MB" -c 'test "" -o x && echo yes || echo no')" "yes"

# -----------------------------------------------------------------------
# "$*" joins positionals with the FIRST byte of IFS: a space when IFS is
# unset, but NOTHING when IFS is set to empty. silex used a space in both cases.
# -----------------------------------------------------------------------
check '"$*" with empty IFS joins with no separator' \
    "$("$MB" -c 'set -- ab c def; IFS=; echo "[$*]"')" "[abcdef]"
check '"$*" with unset IFS joins with a space' \
    "$("$MB" -c 'set -- ab c def; unset IFS; echo "[$*]"')" "[ab c def]"
check '"$*" with IFS=: joins with a colon' \
    "$("$MB" -c 'set -- ab c def; IFS=:; echo "[$*]"')" "[ab:c:def]"

# -----------------------------------------------------------------------
# Here-document bodies: expansions run but `'` and `"` are literal, and $*/$@
# are joined (a here-doc is text, never field-split). silex used to process the
# quotes -- a stray `'` swallowed the rest of the line, leaving $HOME unexpanded
# -- and leaked internal 0x01 bytes for $*/$@.
# -----------------------------------------------------------------------
check "here-doc keeps quotes literal and still expands \$VAR" \
    "$(HOME=/h "$MB" -c 'cat <<EOF
it'\''s "x" $HOME
EOF')" 'it'\''s "x" /h'
check "here-doc \$* with empty IFS joins with no separator" \
    "$("$MB" -c 'set -- a "b c" d; v=$(IFS=; cat <<-EOF
	$*
	EOF
); printf "[%s]" "$v"')" "[ab cd]"
check "here-doc with quoted delimiter does not expand" \
    "$("$MB" -c 'cat <<'\''EOF'\''
$HOME x
EOF')" '$HOME x'

# -----------------------------------------------------------------------
# cd -L (default) traverses logically: $PWD keeps the symlink name used to
# arrive, and "dir/.." cancels textually. cd -P resolves symlinks physically.
# silex used to resolve physically either way (modernish's BUG_CDNOLOGIC).
# -----------------------------------------------------------------------
CDT=$(mktemp -d)
mkdir -p "$CDT/a/b"; ln -sfn "$CDT/a/b" "$CDT/link"
check "cd into a symlink keeps the logical PWD" \
    "$("$MB" -c "cd $CDT/link; echo \$PWD")" "$CDT/link"
check "cd -L .. cancels the symlink component textually" \
    "$("$MB" -c "cd $CDT/link; cd -L ..; echo \$PWD")" "$CDT"
check "cd -P .. follows the symlink physically" \
    "$("$MB" -c "cd $CDT/link; cd -P ..; echo \$PWD")" "$CDT/a"
rm -rf "$CDT"

# -----------------------------------------------------------------------
# Bash indirect/prefix expansion (${!name}, ${!name@}, ${!name*}) is not POSIX;
# silex rejects it as a bad substitution like dash, rather than silently
# mis-parsing it (which made modernish's var.t wrongly detect VARPREFIX). The
# `${!}` last-background-PID special parameter stays valid.
# -----------------------------------------------------------------------
check "\${!name} indirect expansion is a bad substitution" \
    "$("$MB" -c 'foo=x; echo "${!foo}"' 2>&1 >/dev/null | grep -c 'bad substitution')" "1"
check "\${!name@} prefix expansion is a bad substitution" \
    "$("$MB" -c 'foo=x; echo "${!foo@}"' 2>&1 >/dev/null | grep -c 'bad substitution')" "1"
check "\${!} is still the last-background-PID special parameter" \
    "$("$MB" -c 'sleep 0 & p=${!}; case $p in ([0-9]*) echo numeric;; (*) echo no;; esac')" "numeric"

# -----------------------------------------------------------------------
# Positional-parameter expansion must survive LITERAL control bytes that
# collide with silex's internal markers: 0x01 ("$@"/"$*" field boundary) and
# 0x02/0x03 (quote guards). silex escapes reserved bytes in expanded data and
# decodes each field afterwards. modernish's posparam.t injects exactly these.
# CB = 0x01 0x02 0x03 0x7f; hexdump of "abc:d:<CB>" is 6162633a643a0102037f.
# -----------------------------------------------------------------------
CB=$(printf '\001\002\003\177')
check "\$@ field count is right despite a literal 0x01 in a parameter, IFS empty" \
    "$("$MB" -c 'set -- a "$1" b; IFS=; set -- $@; echo "$#"' _ "$CB")" "3"
check "quoted \"\$@\" preserves a parameter containing control bytes" \
    "$("$MB" -c 'set -- a "$1" b; set -- "$@"; printf "%s" "$2" | od -An -tx1 | tr -d " \n"' _ "$CB")" \
    "0102037f"
check "\${var-\"\$*\"} keeps control bytes and stays one field (IFS=:)" \
    "$("$MB" -c 'set -- abc "d" "$1"; unset var; IFS=:; set -- ${var-"$*"}; echo "$#"' _ "$CB")" "1"
check "\${var-\"\$*\"} value has the control bytes intact (IFS=:)" \
    "$("$MB" -c 'set -- abc "d" "$1"; unset var; IFS=:; set -- ${var-"$*"}; printf "%s" "$1" | od -An -tx1 | tr -d " \n"' _ "$CB")" \
    "6162633a643a0102037f"
check "\${var=\$*} assigns the DECODED value, no internal escape bytes" \
    "$("$MB" -c 'set -- abc "d" "$1"; unset var; IFS=:; : ${var=$*}; printf "%s" "$var" | od -An -tx1 | tr -d " \n"' _ "$CB")" \
    "6162633a643a0102037f"

# -----------------------------------------------------------------------
# `read` must consume no more than one line from its fd (POSIX). silex used
# buffered fgetc(), which slurped the whole block, so a later reader of the same
# fd saw nothing and re-exec'ing a fd kept reading the old one. This is also
# what wedged modernish's nested LOOP (its generators pipe over fd 8).
# -----------------------------------------------------------------------
printf 'l1\nl2\nl3\n' > "$TMPD/rd"
check "read leaves the rest of the fd for the next reader" \
    "$("$MB" -c "exec 8<$TMPD/rd; read a<&8; printf '%s,' \"\$a\"; cat <&8 | tr '\\n' '/'")" \
    "l1,l2/l3/"
printf 'o1\no2\n' > "$TMPD/o"; printf 'i1\ni2\n' > "$TMPD/i"
check "re-exec of a fd is honoured by the next read" \
    "$("$MB" -c "exec 8<$TMPD/o; read a<&8; exec 8<$TMPD/i; read b<&8; echo \"\$a \$b\"")" \
    "o1 i1"

# -----------------------------------------------------------------------
# Command substitution must expand aliases (POSIX; dash does). silex ran the
# $(...) body in a fresh subshell without inheriting the parent's aliases, so
# modernish's alias-based DSL (LOOP/DO/DONE, not, ...) was "command not found"
# inside $(...) -- which broke nested LOOP (its body runs in a command sub).
# -----------------------------------------------------------------------
check "an alias expands inside a command substitution" \
    "$(printf 'alias GREET=%s\nv=$(GREET)\nprintf "[%%s]" "$v"\n' "'echo hi'" | "$MB")" \
    "[hi]"

# -----------------------------------------------------------------------
# `trap` must reject an unknown condition ("bad trap", non-zero) like dash, and
# must arm/list ALL real signals -- not the handful it used to. silex accepted
# any name (ERR/ZERR/BOGUS) and silently dropped real ones (ALRM/CONT), so
# modernish detected nonexistent signals like ZERR.
# -----------------------------------------------------------------------
check "trap rejects an unknown condition with non-zero status" \
    "$("$MB" -c "trap 'x' ZERR 2>/dev/null; echo \$?")" "1"
check "trap rejects a bogus signal name" \
    "$("$MB" -c "trap 'x' BOGUS 2>/dev/null; echo \$?")" "1"
check "trap arms and lists a real signal (ALRM) that was previously dropped" \
    "$("$MB" -c "trap 'echo a' ALRM; trap")" "trap -- 'echo a' ALRM"
check "trap on a valid signal still succeeds" \
    "$("$MB" -c "trap 'echo i' INT; echo \$?")" "0"

# -----------------------------------------------------------------------
# A function's exit status shares no channel with flow control: `return N` for
# N in 200-255 used to collide with the break/continue/return sentinels, so the
# function (or whole shell) misbehaved. `return 255` in particular exited the
# shell -- which silently swallowed modernish's mapr callback aborting with 255.
# -----------------------------------------------------------------------
check "return 255 sets the status and does NOT exit the shell" \
    "$("$MB" -c 'f() { return 255; }; f; echo "A=$?"')" "A=255"
check "return 200 (was the break sentinel) is a real status" \
    "$("$MB" -c 'f() { return 200; }; f; echo "A=$?"')" "A=200"
check "return 202 (was the return sentinel) is a real status" \
    "$("$MB" -c 'f() { return 202; }; f; echo "A=$?"')" "A=202"
check "return 255 propagates through || like any failure" \
    "$("$MB" -c 'f() { return 255; }; f || echo "caught=$?"')" "caught=255"
check "break still breaks a loop" \
    "$("$MB" -c 'for i in 1 2 3; do [ "$i" = 2 ] && break; echo "$i"; done; echo end')" \
    "$(printf '1\nend')"
check "eval return is transparent to the enclosing function" \
    "$("$MB" -c 'f() { eval "return 7"; echo NOPE; }; f; echo "g=$?"')" "g=7"

# -----------------------------------------------------------------------
# Pathname expansion of a word that MIXES a quoted region with an unquoted
# glob-bearing expansion: `"$dir"/$pat` (pat='*.x') must glob the unquoted `*`.
# silex used to disable globbing for the whole word once any part was quoted, so
# modernish's countfiles() (`set -- "$dir"/$pat`) counted zero -> builtin.t 029.
# -----------------------------------------------------------------------
GT=$(mktemp -d); : > "$GT/one.x"; : > "$GT/two.x"; : > "$GT/skip.y"
check '"$dir"/$pat globs the unquoted pattern from a variable' \
    "$("$MB" -c 'd=$1; p="*.x"; set -- "$d"/$p; echo $#' _ "$GT")" "2"
check 'a fully-quoted "$dir/$pat" does NOT glob' \
    "$("$MB" -c 'd=$1; p="*.x"; set -- "$d/$p"; echo $#' _ "$GT")" "1"
check 'a quoted literal "*" still never globs' \
    "$("$MB" -c 'cd "$1"; set -- "*.x"; echo "$#:$1"' _ "$GT")" '1:*.x'
check 'unquoted $dir/$pat still globs' \
    "$("$MB" -c 'd=$1; p="*.x"; set -- $d/$p; echo $#' _ "$GT")" "2"
rm -rf "$GT"

# -----------------------------------------------------------------------
echo
echo "modernish-bringup tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
