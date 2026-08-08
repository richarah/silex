#!/bin/sh
# tests/unit/shell/test_command_by_command.sh
#
# Regression tests for the command-by-command (line-granularity) parsing work
# and the bugs it uncovered while getting modernish's LOOP/DO/DONE and its
# `let` helper to run under silex:
#
#   1. Line-granularity parsing: a whole file/`-c` string is no longer parsed up
#      front. Each logical line is parsed, executed, then the next is parsed, so
#      an alias/`use`/function defined by one line is visible to the next
#      (POSIX, matches dash). A `;`-separated alias on the SAME line is still not
#      yet in effect (matches dash).
#   2. Sourced-file stdio corruption: reading a script with buffered stdio and
#      forking a child between commands let the child's exit-time stdio cleanup
#      lseek the shared fd backward, splicing earlier file content into a later
#      line. Sourced files are now slurped into memory.
#   3. `test`/`[` file-type operators -p -b -c -S -h -g -u -k -O -G -t.
#   4. Arithmetic: a parameter whose VALUE is itself an expression (a positional
#      `$1`="i = (1)" or `v="2 + 3"` used as `($v)`) must be expanded and
#      evaluated, not treated as a single number-or-name (which gave 0).
#
# Usage: ./test_command_by_command.sh [path/to/silex]

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
# 1. Line-granularity alias visibility
# -----------------------------------------------------------------------
# Across a newline, an alias defined on the previous line IS in effect.
printf 'alias greet="echo hi"\ngreet\n' > "$TMPD/cross.sh"
check "alias defined on a prior line expands on the next" \
    "$("$MB" "$TMPD/cross.sh" 2>&1)" "hi"

# On the SAME line (`;`-separated), the alias is not yet in effect (like dash):
check "alias on the same line is not yet in effect" \
    "$("$MB" -c 'alias zz="echo Z"; zz 2>/dev/null || echo notfound')" "notfound"

# A keyword-forming alias that spans lines: the block alias opens a brace group
# a later line closes. Mirrors modernish's LOOP/DO/DONE.
cat > "$TMPD/block.sh" <<'EOF'
alias OPEN='{ echo start'
alias CLOSE='echo end; }'
OPEN
echo middle
CLOSE
EOF
check "block-opening alias closed by a later line's alias" \
    "$("$MB" "$TMPD/block.sh" 2>&1)" "$(printf 'start\nmiddle\nend')"

# A function defined on one line is callable on the next (already worked, guard).
printf 'f() { echo fn; }\nf\n' > "$TMPD/fn.sh"
check "function defined on a prior line is callable next" \
    "$("$MB" "$TMPD/fn.sh" 2>&1)" "fn"

# -----------------------------------------------------------------------
# 2. Sourced-file stdio corruption (fork between commands)
# -----------------------------------------------------------------------
# A sourced file whose early command forks a child (here a subshell/command
# substitution), followed by many later lines, must not re-read earlier content.
# Before the slurp fix the buffered fd was rewound by the child's exit and the
# tokenizer spliced the header back into a later line.
{
    echo 'x=$( (echo one) )'          # forks a subshell + command substitution
    i=0
    while [ "$i" -lt 200 ]; do
        echo "v$i=line_$i"
        i=$((i+1))
    done
    echo 'echo "$x $v0 $v199"'
} > "$TMPD/sourced.sh"
printf '. "%s"\n' "$TMPD/sourced.sh" > "$TMPD/driver.sh"
check "sourced file with an early fork parses cleanly to the end" \
    "$("$MB" "$TMPD/driver.sh" 2>&1)" "one line_0 line_199"

# -----------------------------------------------------------------------
# 3. test / [ file-type operators
# -----------------------------------------------------------------------
mkfifo "$TMPD/fifo" 2>/dev/null
check "test -p detects a FIFO" \
    "$("$MB" -c '[ -p "$1" ] && echo yes || echo no' _ "$TMPD/fifo")" "yes"
check "test -p is false for a regular file" \
    "$("$MB" -c 'echo hi > "$1"; [ -p "$1" ] && echo yes || echo no' _ "$TMPD/reg")" "no"
check "test -c detects a char device (/dev/null)" \
    "$("$MB" -c '[ -c /dev/null ] && echo yes || echo no')" "yes"
check "test -h detects a symlink" \
    "$("$MB" -c 'ln -s /nowhere "$1"; [ -h "$1" ] && echo yes || echo no' _ "$TMPD/lnk")" "yes"

# -----------------------------------------------------------------------
# 4. Arithmetic: a parameter whose value is itself an expression
# -----------------------------------------------------------------------
check "arith: variable holding an expression, parenthesised" \
    "$("$MB" -c 'v="2 + 3"; echo $(( ($v) * 2 ))')" "10"
check "arith: positional holding an expression" \
    "$("$MB" -c 'set -- "2 + 3"; echo $(( ($1) * 2 ))')" "10"
# The exact shape of modernish's 3-argument `let` (arithmetic loop setup):
check "arith: modernish let 3-arg return expression evaluates to success" \
    "$("$MB" -c 'f() { return "$((($1)&($2)&0|!($3)))"; }; f "i = (1)" "j = (3)" "k = (i > j ? -1 : 1)"; echo $?')" "0"
check "arith: let-style 1-arg true predicate returns 0" \
    "$("$MB" -c 'n=3; f() { return "$((!($1)))"; }; f "n >= 3"; echo $?')" "0"
check "arith: let-style 1-arg false predicate returns 1" \
    "$("$MB" -c 'n=1; f() { return "$((!($1)))"; }; f "n >= 3"; echo $?')" "1"

# -----------------------------------------------------------------------
echo ""
echo "command-by-command tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
