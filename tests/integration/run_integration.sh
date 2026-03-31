#!/bin/bash
# tests/integration/run_integration.sh -- Integration test runner
#
# Builds sample shell scripts using silex as /bin/sh, comparing output
# against dash as reference. Validates exit codes and stdout equivalence.
#
# Usage: ./run_integration.sh [SILEX_BINARY]

set -uo pipefail

SILEX="${1:-build/bin/silex}"
PASS=0
FAIL=0
SKIP=0

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found: $SILEX" >&2
    exit 1
fi

# Reference shell for comparison
REF_SH="dash"
if ! command -v dash >/dev/null 2>&1; then
    REF_SH="/bin/sh"
fi

TMPDIR_INT=$(mktemp -d)
trap 'rm -rf "$TMPDIR_INT"' EXIT INT TERM

tap_test() {
    local desc="$1"
    local script="$2"

    local mb_out mb_exit ref_out ref_exit

    mb_out=$(echo "$script" | "$SILEX" /dev/stdin 2>&1)
    mb_exit=$?
    ref_out=$(echo "$script" | "$REF_SH" 2>&1)
    ref_exit=$?

    if [ "$mb_out" = "$ref_out" ] && [ "$mb_exit" = "$ref_exit" ]; then
        echo "ok - $desc"
        PASS=$((PASS+1))
    else
        echo "not ok - $desc"
        if [ "$mb_exit" != "$ref_exit" ]; then
            echo "  exit: got=$mb_exit expected=$ref_exit"
        fi
        if [ "$mb_out" != "$ref_out" ]; then
            echo "  expected: $(echo "$ref_out" | head -3)"
            echo "  got:      $(echo "$mb_out" | head -3)"
        fi
        FAIL=$((FAIL+1))
    fi
}

echo "TAP version 13"
echo "# Integration tests: silex vs $REF_SH"
echo "# silex: $SILEX"

# ---------------------------------------------------------------------------
# Basic shell scripts
# ---------------------------------------------------------------------------

tap_test "simple echo" 'echo hello world'
tap_test "variable assignment" 'X=hello; echo $X'
tap_test "arithmetic" 'echo $((2 + 3))'
tap_test "command substitution" 'X=$(echo hello); echo $X'
tap_test "pipe" 'echo hello | cat'
tap_test "if true" 'if true; then echo yes; fi'
tap_test "if false" 'if false; then echo yes; else echo no; fi'
tap_test "for loop" 'for i in 1 2 3; do echo $i; done'
tap_test "while loop" 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
tap_test "case statement" 'case hello in hello) echo match;; *) echo no;; esac'
tap_test "function definition" 'f() { echo called; }; f'
tap_test "exit code" 'true; echo $?'
tap_test "negation" '! false; echo $?'
tap_test "and operator" 'true && echo yes'
tap_test "or operator" 'false || echo fallback'
tap_test "redirect to file" "echo test > $TMPDIR_INT/redir.txt; cat $TMPDIR_INT/redir.txt"
tap_test "redirect append" "echo a > $TMPDIR_INT/app.txt; echo b >> $TMPDIR_INT/app.txt; cat $TMPDIR_INT/app.txt"
tap_test "heredoc" 'cat <<EOF
hello
world
EOF'
tap_test "parameter default" 'X=; echo ${X:-default}'
tap_test "parameter length" 'X=hello; echo ${#X}'
tap_test "parameter prefix strip" 'X=/usr/local/bin; echo ${X#/usr}'
tap_test "IFS splitting" 'IFS=:; for f in a:b:c; do echo $f; done'
tap_test "quoted glob" 'echo "*"'
tap_test "subshell" '(X=inner; echo $X); echo ${X:-outer}'

# ---------------------------------------------------------------------------
# Builtin tests
# ---------------------------------------------------------------------------

tap_test "builtin echo -n" 'echo -n hello; echo'
tap_test "builtin mkdir" "mkdir -p $TMPDIR_INT/int_dir; echo created"
tap_test "builtin cp" "cp /etc/hostname $TMPDIR_INT/host.txt; echo ok"
tap_test "builtin cat" 'echo test | cat'
tap_test "builtin wc -l" 'printf "a\nb\nc\n" | wc -l'
tap_test "builtin sort" 'printf "c\na\nb\n" | sort'
tap_test "builtin grep" 'printf "aa\nbb\ncc\n" | grep bb'
tap_test "builtin sed" 'echo hello | sed s/hello/world/'
tap_test "builtin basename" 'basename /usr/local/bin'
tap_test "builtin dirname" 'dirname /usr/local/bin'
tap_test "builtin tr" 'echo hello | tr a-z A-Z'
tap_test "builtin cut" 'echo a:b:c | cut -d: -f2'

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

TOTAL=$((PASS+FAIL+SKIP))
echo ""
echo "1..$TOTAL"
echo "# Results: $PASS passed, $FAIL failed, $SKIP skipped out of $TOTAL"
[ "$FAIL" -eq 0 ]
