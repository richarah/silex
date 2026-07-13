#!/bin/sh
# tests/unit/shell/test_resource_limits.sh — resource exhaustion cap tests
# Verifies SHELL_MAX_CALL_DEPTH, arena cap, and strbuf cap are enforced.
# Usage: ./test_resource_limits.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
PASS=0
FAIL=0

# The recursion-depth tests below are meaningless under AddressSanitizer.
#
# They assert that silex hits its own SHELL_MAX_CALL_DEPTH cap and reports a
# depth error. ASan inflates every stack frame (redzones, shadow bookkeeping) by
# a large factor, so the process runs out of real stack and is killed by the
# kernel long before the shell's own counter reaches the cap. The shell never
# gets to print its message, and it does not survive to run another command --
# so both "no depth error in stderr" and "shell continues after depth error"
# fail. That is ASan's stack, not a silex bug.
#
# Detected by asking the binary, not by an env var, so it is correct whether the
# sanitiser came from `make debug` or from CFLAGS on the command line.
SANITIZED=0
if command -v nm >/dev/null 2>&1 && nm -D "$SILEX" 2>/dev/null | grep -q '__asan_'; then
    SANITIZED=1
elif command -v strings >/dev/null 2>&1 && strings "$SILEX" 2>/dev/null | grep -q 'AddressSanitizer'; then
    SANITIZED=1
fi

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
timeout 5 "$SILEX" -c '
f() { f; }
f
' >/dev/null 2>&1
check_exit "recursion: direct recursion exits non-zero" "$?" "1"

# Mutual recursion: a calls b, b calls a — must hit cap and exit non-zero
timeout 5 "$SILEX" -c '
a() { b; }
b() { a; }
a
' >/dev/null 2>&1
check_exit "recursion: mutual recursion exits non-zero" "$?" "1"

# Error message must mention call depth.
# Skipped under ASan: the process dies on a real stack overflow before the
# shell's own depth counter trips, so there is no message to check.
if [ "$SANITIZED" -eq 1 ]; then
    echo "SKIP: recursion depth message (ASan inflates stack frames)"
else
got=$(timeout 5 "$SILEX" -c '
f() { f; }
f
' 2>&1 | grep -ci "call depth\|maximum")
if [ "${got:-0}" -ge 1 ] 2>/dev/null; then
    echo "PASS: recursion: error message mentions call depth"; PASS=$((PASS+1))
else
    echo "FAIL: recursion: no depth error in stderr"; FAIL=$((FAIL+1))
fi

# Shell must not hang — must terminate within timeout (no stack overflow)
timeout 5 "$SILEX" -c '
f() { f; }
f
' >/dev/null 2>&1
check_exit "recursion: terminates without hang" "$?" "1"

# Recursion error returns non-zero; subsequent commands still execute
got=$(timeout 5 "$SILEX" -c '
f() { f; }
f
echo "continued"
' 2>/dev/null)
check "recursion: shell continues after depth error" "$got" "continued"
fi   # end: skipped under ASan

# Recursion counter resets between separate invocations (no state leak)
timeout 5 "$SILEX" -c 'f() { f; }; f' >/dev/null 2>&1; rc1=$?
timeout 5 "$SILEX" -c 'echo ok' 2>/dev/null; rc2=$?
if [ "$rc1" -ne 0 ] && [ "$rc2" -eq 0 ]; then
    echo "PASS: recursion: counter isolated between invocations"; PASS=$((PASS+1))
else
    echo "FAIL: recursion: counter leaked between invocations ($rc1, $rc2)"; FAIL=$((FAIL+1))
fi

# -----------------------------------------------------------------------
# String buffer cap (SB_MAX_CAP = 64 MB)
# -----------------------------------------------------------------------

# Moderate string (1000 chars): no truncation
got=$("$SILEX" -c 'x=$(head -c 1000 /dev/zero | tr "\0" "A"); echo ${#x}' 2>/dev/null)
check "strbuf: 1000-char string length is 1000" "$got" "1000"

# String that grows via repeated append stays correct
got=$("$SILEX" -c '
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
got=$("$SILEX" -c '
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
