#!/bin/bash
# test_grep.sh — grep match bounds, context and -o/--color regressions.
# Expected outputs are what GNU grep 3.11 prints.
# Usage: test_grep.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
case "$SILEX" in /*) ;; *) SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;; esac
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

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf 'a\nx\na\ny\nz\n' > "$T/f"
printf 'a\nb\n' > "$T/g"
G() { "$SILEX" grep "$@" | paste -sd, -; }

echo "--- after-context (-A) ---"
# The first-char prefilter skipped lines without "a" outright, so they
# were never printed as after-context.
check "-A1 prints the line after each match" "a,x,a,y" "$(G -A1 a "$T/f")"
check "-A1 with a regex" "a,x,a,y" "$(G -A1 'a.*' "$T/f")"
check "-A1 from stdin" "a,x,a,y" "$(printf 'a\nx\na\ny\nz\n' | "$SILEX" grep -A1 a | paste -sd, -)"
check "-m1 -A1 keeps the trailing context" "a,x" "$(G -m1 -A1 a "$T/f")"
check "-m1 -A3: a later match is printed as context" "1:a,2-x,3-a,4-y" "$(G -n -m1 -A3 a "$T/f")"

echo "--- before-context and separators ---"
check "-B1: touching groups get no --" "a,x,a" "$(G -B1 a "$T/f")"
check "-C1: one group, no --" "a,x,a,y" "$(G -C1 a "$T/f")"
check "-- between files" "$T/g:a,$T/g-b,--,$T/f:a,$T/f-x,$T/f:a,$T/f-y" "$(G -A1 a "$T/g" "$T/f")"
check "-- between files is reset on every run (one silex sh)" \
    "a,x,a,y|a,x,a,y" \
    "$("$SILEX" sh -c "grep -A1 a '$T/f' | paste -sd, -; grep -A1 a '$T/f' | paste -sd, -" | paste -sd'|' -)"

echo "--- match bounds: leftmost-longest ---"
check "-o a.* prints the whole match" "abc" "$(echo abc | "$SILEX" grep -o 'a.*')"
check "-o [a-z]* skips empty matches" "ab,cd" "$(echo 'ab cd' | "$SILEX" grep -o '[a-z]*' | paste -sd, -)"
check "-x a.* matches ab" "ab" "$(echo ab | "$SILEX" grep -x 'a.*')"
check "-xE b+ matches bbb" "bbb" "$(echo bbb | "$SILEX" grep -xE 'b+')"
check "-w a.* matches ab" "ab" "$(echo ab | "$SILEX" grep -w 'a.*')"

echo "--- anchors ---"
check "\$ alone matches every line" "5" "$("$SILEX" grep -c '$' "$T/f")"
check "-o a\$ (libc-backed pattern)" "a" "$(echo xaya | "$SILEX" grep -o 'a$')"
check "-o ^a matches only at the start" "a" "$(echo aaa | "$SILEX" grep -o '^a' | paste -sd, -)"

echo "--- intervals and BRE anchors ---"
check "BRE \\{0,1\\} allows none" "1" "$(echo ac | "$SILEX" grep -c 'ab\{0,1\}c')"
check "BRE \\{0,1\\} rejects two" "0" "$(echo abbc | "$SILEX" grep -c 'ab\{0,1\}c')"
check "ERE x{2} needs two" "0" "$(echo x | "$SILEX" grep -cE 'x{2}')"
check "ERE -o a{2,3}" "aaa" "$(echo aaaa | "$SILEX" grep -oE 'a{2,3}')"
check "ERE a{ is a literal brace" "1" "$(echo 'a{' | "$SILEX" grep -cE 'a{')"
check "ERE x{,1}y (GNU {,n})" "xy,y" "$(printf 'xy\ny\nxxy\n' | "$SILEX" grep -xE 'x{,1}y' | paste -sd, -)"
check "BRE a^b is literal" "1" "$(echo 'a^b' | "$SILEX" grep -c 'a^b')"
check "BRE x*^ needs a literal ^" "0" "$(echo c | "$SILEX" grep -c 'x*^')"

echo "--- -o and --color with several patterns, -w ---"
check "-o uses every -e pattern" "bar,foo" "$(echo 'bar foo' | "$SILEX" grep -o -e foo -e bar | paste -sd, -)"
check "-o prefers the longest match at a position" "foob" "$(echo foobar | "$SILEX" grep -o -e foo -e foob)"
check "-ow prints whole words only" "foo" "$(echo 'xfoo foo' | "$SILEX" grep -ow foo | paste -sd, -)"
check "-o prints no context lines" "a,a" "$(G -o -A1 a "$T/f")"
E=$(printf '\033')
check "--color writes the rest of the line once" "a${E}[01;31mb${E}[0mc" "$(echo abc | "$SILEX" grep --color=always b)"
check "--color highlights every -e pattern" "${E}[01;31mbar${E}[0m ${E}[01;31mfoo${E}[0m" \
    "$(echo 'bar foo' | "$SILEX" grep --color=always -e foo -e bar)"
check "--color on a libc-backed pattern" "x${E}[01;31ma${E}[0m" "$(echo xa | "$SILEX" grep --color=always 'a$')"

echo ""
echo "grep: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
