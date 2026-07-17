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
# Loop iterations must not accumulate scratch memory
#
# A loop is a single top-level command, so before the per-loop arena every
# iteration's expansions leaked: ~220 bytes each, 46 MB by 200k iterations, and
# an abort at the 64 MB arena cap somewhere between 200k and 300k. 300k is used
# below because it is past that cap: on the old allocator this aborts.
# -----------------------------------------------------------------------

got=$(timeout 60 "$SILEX" -c '
i=0
while [ $i -lt 300000 ]; do
    i=$((i+1))
done
echo $i
' 2>/dev/null)
check "arena: 300k-iteration while loop completes" "$got" "300000"

got=$(timeout 60 "$SILEX" -c '
i=0
until [ $i -ge 300000 ]; do
    i=$((i+1))
done
echo $i
' 2>/dev/null)
check "arena: 300k-iteration until loop completes" "$got" "300000"

# The loop body must not abort with an arena error on stderr.
got=$(timeout 60 "$SILEX" -c '
i=0
while [ $i -lt 300000 ]; do
    i=$((i+1))
done
' 2>&1 | grep -ci 'arena')
check "arena: long loop reports no arena error" "${got:-0}" "0"

# Memory must be FLAT in the iteration count, not merely smaller. Comparing two
# sizes catches a leak that a single threshold would let through.
#
# Meaningless under AddressSanitizer, for the same reason as the recursion tests
# above: it asserts on process RSS, and under ASan RSS is dominated by the
# quarantine, which holds freed chunks back in proportion to how many
# allocations ran -- so it climbs with the iteration count no matter how flat
# the arena is. Measured on this loop: 156 MB -> 487 MB across 50k -> 400k with
# the default quarantine, but 24364 KB -> 24904 KB (flat) with
# quarantine_size_mb=1. That is ASan's allocator, not a silex leak. The
# non-sanitised CI jobs run this same assertion for real.
if [ "$SANITIZED" -eq 1 ]; then
    echo "SKIP: arena: loop RSS flat (ASan quarantine dominates RSS)"
elif command -v /usr/bin/time >/dev/null 2>&1; then
    rss_small=$(/usr/bin/time -f '%M' "$SILEX" -c \
        'i=0; while [ $i -lt 50000 ]; do i=$((i+1)); done' 2>&1 | tail -1)
    rss_large=$(/usr/bin/time -f '%M' "$SILEX" -c \
        'i=0; while [ $i -lt 400000 ]; do i=$((i+1)); done' 2>&1 | tail -1)
    # 8x the iterations must not cost 2x the memory. The old code grew ~linearly
    # (13 MB -> 67 MB over this range), so it would fail this by a wide margin.
    if [ "${rss_small:-0}" -gt 0 ] 2>/dev/null && [ "${rss_large:-0}" -gt 0 ] 2>/dev/null; then
        if [ "$rss_large" -lt $((rss_small * 2)) ]; then
            echo "PASS: arena: loop RSS flat across 50k->400k (${rss_small}K -> ${rss_large}K)"
            PASS=$((PASS+1))
        else
            echo "FAIL: arena: loop RSS grew with iterations (${rss_small}K -> ${rss_large}K)"
            FAIL=$((FAIL+1))
        fi
    else
        echo "SKIP: arena: RSS measurement unavailable"
    fi
else
    echo "SKIP: arena: /usr/bin/time not available for RSS check"
fi

# Reclaiming per iteration must not dangle anything the loop still owns.
# Each of these reads memory allocated on an earlier iteration or before the
# loop; all of them regress if the loop arena is reset too aggressively.
got=$("$SILEX" -c '
i=0
while [ $i -lt 3 ]; do i=$((i+1)); set -- "arg$i"; done
echo "$1"
' 2>/dev/null)
check "arena: set -- inside loop survives the loop" "$got" "arg3"

got=$("$SILEX" -c '
f() { j=0; while [ $j -lt 3 ]; do j=$((j+1)); done; echo "$1"; }
f keepme
' 2>/dev/null)
check "arena: function positionals survive an inner loop" "$got" "keepme"

got=$("$SILEX" -c 'for w in a b c; do printf "%s" "$w"; done' 2>/dev/null)
check "arena: for word list survives its own loop" "$got" "abc"

got=$("$SILEX" -c '
for o in x y; do k=0; while [ $k -lt 2 ]; do k=$((k+1)); done; printf "%s" "$o"; done
' 2>/dev/null)
check "arena: outer for word survives nested inner loop" "$got" "xy"

# eval/./trap re-enter the interpreter while the caller is mid-command. If they
# reclaim the caller's arena instead of their own, the enclosing loop's word
# list is freed underneath it: this printed "alpha pha" and then stopped early,
# silently corrupting rather than crashing, because a reset arena keeps its
# blocks mapped and the stale read usually still finds the old bytes.
got=$("$SILEX" -c 'for x in alpha bravo charlie; do eval "true"; printf "%s " "$x"; done' 2>/dev/null)
check "arena: eval inside for loop does not corrupt the word list" "$got" "alpha bravo charlie "

# Same, with an eval that allocates enough to actually reuse the freed region.
got=$("$SILEX" -c '
for x in alpha bravo charlie; do
    eval "z=BBBBBBBBBBBBBBBBBBBBBBBB; w=\"\$z\$z\$z\$z\"; echo \"\$w\" > /dev/null"
    printf "%s " "$x"
done' 2>/dev/null)
check "arena: allocating eval inside for loop preserves word list" "$got" "alpha bravo charlie "

got=$("$SILEX" -c 'i=0; while [ $i -lt 3 ]; do i=$((i+1)); eval "true"; done; echo $i' 2>/dev/null)
check "arena: eval inside while loop" "$got" "3"

got=$("$SILEX" -c '
f() { for x in p q; do eval "true"; done; echo "$1"; }
f keepme' 2>/dev/null)
check "arena: eval in loop keeps enclosing function positionals" "$got" "keepme"

# `.` re-enters through shell_run_file, the same hazard as eval.
_tmpsrc=$(mktemp 2>/dev/null || echo /tmp/silex_src_$$)
echo 'true' > "$_tmpsrc"
got=$("$SILEX" -c "for x in alpha bravo charlie; do . $_tmpsrc; printf '%s ' \"\$x\"; done" 2>/dev/null)
check "arena: sourcing a file inside for loop preserves word list" "$got" "alpha bravo charlie "
rm -f "$_tmpsrc"

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------

echo ""
echo "resource limit tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
