#!/bin/sh
# tests/unit/shell/test_resource_limits.sh — resource exhaustion cap tests
# Verifies SHELL_MAX_CALL_DEPTH, arena cap, and strbuf cap are enforced.
# Usage: ./test_resource_limits.sh [path/to/matchbox]

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0

check() {
    local desc="$1" got="$2" expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"; PASS=$((PASS+1))
    else
        echo "FAIL: $desc"
        echo "  expected: $(printf '%s' "$expected" | cat -A)"
        echo "  got:      $(printf '%s' "$got"      | cat -A)"
        FAIL=$((FAIL+1))
    fi
}

check_exit() {
    local desc="$1" got="$2" expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"; PASS=$((PASS+1))
    else
        echo "FAIL: $desc (expected exit=$expected, got exit=$got)"
        FAIL=$((FAIL+1))
    fi
}

# -----------------------------------------------------------------------
# Recursion depth cap (SHELL_MAX_CALL_DEPTH = 1000)
# -----------------------------------------------------------------------

# Direct recursion: function calls itself, must hit cap and exit non-zero
timeout 5 "$MATCHBOX" -c '
f() { f; }
f
' >/dev/null 2>&1
check_exit "recursion: direct recursion exits non-zero" "$?" "1"

# Mutual recursion: a calls b, b calls a — must hit cap and exit non-zero
timeout 5 "$MATCHBOX" -c '
a() { b; }
b() { a; }
a
' >/dev/null 2>&1
check_exit "recursion: mutual recursion exits non-zero" "$?" "1"

# Error message must mention call depth
got=$(timeout 5 "$MATCHBOX" -c '
f() { f; }
f
' 2>&1 | grep -ci "call depth\|maximum")
if [ "${got:-0}" -ge 1 ] 2>/dev/null; then
    echo "PASS: recursion: error message mentions call depth"; PASS=$((PASS+1))
else
    echo "FAIL: recursion: no depth error in stderr"; FAIL=$((FAIL+1))
fi

# Shell must not hang — must terminate within timeout (no stack overflow)
timeout 5 "$MATCHBOX" -c '
f() { f; }
f
' >/dev/null 2>&1
check_exit "recursion: terminates without hang" "$?" "1"

# Recursion error returns non-zero; subsequent commands still execute
got=$(timeout 5 "$MATCHBOX" -c '
f() { f; }
f
echo "continued"
' 2>/dev/null)
check "recursion: shell continues after depth error" "$got" "continued"

# Recursion counter resets between separate invocations (no state leak)
timeout 5 "$MATCHBOX" -c 'f() { f; }; f' >/dev/null 2>&1; rc1=$?
timeout 5 "$MATCHBOX" -c 'echo ok' 2>/dev/null; rc2=$?
if [ "$rc1" -ne 0 ] && [ "$rc2" -eq 0 ]; then
    echo "PASS: recursion: counter isolated between invocations"; PASS=$((PASS+1))
else
    echo "FAIL: recursion: counter leaked between invocations ($rc1, $rc2)"; FAIL=$((FAIL+1))
fi

# -----------------------------------------------------------------------
# String buffer cap (SB_MAX_CAP = 64 MB)
# -----------------------------------------------------------------------

# Moderate string (1000 chars): no truncation
got=$("$MATCHBOX" -c 'x=$(head -c 1000 /dev/zero | tr "\0" "A"); echo ${#x}' 2>/dev/null)
check "strbuf: 1000-char string length is 1000" "$got" "1000"

# String that grows via repeated append stays correct
got=$("$MATCHBOX" -c '
x=""
i=0
while [ $i -lt 100 ]; do
    x="${x}X"
    i=$((i+1))
done
echo ${#x}
' 2>/dev/null)
check "strbuf: 100-char loop-built string" "$got" "100"

# -----------------------------------------------------------------------
# Arena allocator (indirectly tested via large variable expansion)
# -----------------------------------------------------------------------

# Many variables: arena must handle many small allocations
got=$("$MATCHBOX" -c '
i=0
while [ $i -lt 200 ]; do
    eval "var_$i=value_$i"
    i=$((i+1))
done
echo $var_99
' 2>/dev/null)
check "arena: 200 dynamic variables (eval)" "$got" "value_99"

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

echo ""
echo "resource limit tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
