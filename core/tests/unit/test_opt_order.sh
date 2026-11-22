#!/bin/bash
# test_opt_order.sh — options after operands (GNU getopt ordering)
# Usage: test_opt_order.sh [path/to/silex]
#
# Applets built on the shared parser (src/util/opt.c, OPT_GNU) accept options
# after their operands, as GNU coreutils does: `wc foo -l`, `tail foo -n 5`.
# "--" still ends the options, "-" is still an operand, and POSIXLY_CORRECT
# restores "the first operand ends the options". basename and tr keep GNU's
# stop-at-first-operand behaviour, and self-parsing applets (echo, env) are
# untouched.

set -uo pipefail

SX="${1:-build/bin/silex}"

PASS=0
FAIL=0
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
unset POSIXLY_CORRECT

check() {
    local desc="$1" got="$2" expected="$3"
    if [ "$got" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    got:      $(printf '%q' "$got")"
        echo "    expected: $(printf '%q' "$expected")"
        FAIL=$((FAIL + 1))
    fi
}

printf 'one\ntwo\nthree\nfour\nfive\nsix\n' > foo.txt
printf 'b\na\nc\n' > abc.txt

# --- options after operands ---------------------------------------------
check "wc FILE -l"            "$("$SX" wc foo.txt -l)"             "6 foo.txt"
check "wc FILE --lines"       "$("$SX" wc foo.txt --lines)"        "6 foo.txt"
check "wc -c FILE -l"         "$("$SX" wc -c foo.txt -l)"          "$("$SX" wc -l -c foo.txt)"
check "tail FILE -n 5"        "$("$SX" tail foo.txt -n 5 | tr '\n' ' ')" "two three four five six "
check "tail FILE -n2"         "$("$SX" tail foo.txt -n2 | tr '\n' ' ')"  "five six "
check "tail FILE --lines 1"   "$("$SX" tail foo.txt --lines 1)"    "six"
check "head FILE -n 2"        "$("$SX" head foo.txt -n 2 | tr '\n' ' ')" "one two "
check "head F1 F2 -q -n1"     "$("$SX" head foo.txt abc.txt -q -n1 | tr '\n' ' ')" "one b "
check "cat FILE -n"           "$("$SX" cat abc.txt -n | head -1)"  "$(printf '     1\tb')"
check "sort FILE -r"          "$("$SX" sort abc.txt -r | tr '\n' ' ')" "c b a "
check "cut FILE -c 1"         "$("$SX" cut foo.txt -c 1 | head -2 | tr '\n' ' ')" "o t "
check "grep PAT FILE -c"      "$("$SX" grep o foo.txt -c)"         "3"
check "grep PAT FILE -v -c"   "$("$SX" grep o foo.txt -v -c)"      "3"
check "sed SCRIPT FILE -n"    "$("$SX" sed 2p foo.txt -n)"         "two"
check "dirname NAME -z"       "$("$SX" dirname a/b -z | tr '\0' '@')" "a@"
check "date +FMT -d @0 -u"    "$("$SX" date +%Y -d @0 -u)"         "1970"

mkdir srcd
"$SX" mkdir newd/sub -p
check "mkdir DIR -p"          "$([ -d newd/sub ] && echo yes)"     "yes"
"$SX" cp srcd copyd -r
check "cp SRC DST -r"         "$([ -d copyd ] && echo yes)"        "yes"
"$SX" rm copyd newd -r
check "rm DIR... -r"          "$([ -e copyd ] || [ -e newd ] || echo gone)" "gone"
"$SX" ln foo.txt link -s
check "ln SRC DST -s"         "$([ -L link ] && echo yes)"         "yes"
check "touch FILE -c (no create)" "$("$SX" touch nofile -c; [ -e nofile ] || echo absent)" "absent"

# --- the same through the shell, where applets run in-process --------------
check "sh: wc FILE -l"        "$("$SX" sh -c 'wc foo.txt -l')"    "6 foo.txt"
check "sh: tail FILE -n 1"    "$("$SX" sh -c 'tail foo.txt -n 1')" "six"
check "sh: repeated calls"    "$("$SX" sh -c 'wc foo.txt -l; wc -w foo.txt; wc abc.txt -c')" \
      "$(printf '6 foo.txt\n6 foo.txt\n6 abc.txt')"

# --- "--" and "-" -------------------------------------------------------
printf 'x\n' > ./-l
check "-- ends options"       "$("$SX" wc foo.txt -c -- -l)" \
      "$("$SX" wc -c foo.txt ./-l | sed 's|\./-l|-l|')"
check "-- before operands"    "$("$SX" wc -- -l)"                  "1 1 2 -l"
check "lone - is operand"     "$(printf 'a\n' | "$SX" wc foo.txt - -l)" \
      "$(printf 'a\n' | "$SX" wc -l foo.txt -)"
check "-n argument may look like an operand" \
      "$("$SX" head foo.txt -n 1 abc.txt -q | tr '\n' ' ')" "one b "

# --- errors keep their wording and status --------------------------------
out=$("$SX" wc foo.txt -x 2>&1); rc=$?
check "unknown option after operand: message" "$(echo "$out" | head -1)" \
      "silex: wc: unrecognized option '-x'"
check "unknown option after operand: status" "$rc" "1"
"$SX" tail foo.txt -n >/dev/null 2>&1; rc=$?
check "missing argument after operand: status" "$rc" "1"

# --- POSIXLY_CORRECT: first operand ends the options -----------------------
out=$(POSIXLY_CORRECT=1 "$SX" wc foo.txt -l 2>&1); rc=$?
check "POSIXLY_CORRECT: -l is a file" "$out" "$("$SX" wc -- foo.txt -l 2>&1)"
check "POSIXLY_CORRECT: options first still work" \
      "$(POSIXLY_CORRECT=1 "$SX" wc -l foo.txt)" "6 foo.txt"
out=$(POSIXLY_CORRECT=1 "$SX" tail foo.txt -n 1 2>&1); rc=$?
check "POSIXLY_CORRECT: tail -n is a file (status)" "$rc" "1"
check "sh: POSIXLY_CORRECT prefix reaches builtin applet" \
      "$("$SX" sh -c 'POSIXLY_CORRECT=1 wc foo.txt -l; wc foo.txt -l')" \
      "$("$SX" wc -- foo.txt -l; "$SX" wc -l foo.txt)"

# --- applets that keep stopping at the first operand ----------------------
out=$("$SX" basename a.c -s .c 2>&1); rc=$?
check "basename: -s after NAME is an operand (status)" "$rc" "1"
check "basename: NAME SUFFIX"  "$("$SX" basename a.c .c)"           "a"
check "tr: -d after SET1 is SET2" "$(echo abc | "$SX" tr a -d)"     "-bc"
check "echo: untouched"        "$("$SX" echo foo -n)"               "foo -n"
check "env: untouched"         "$("$SX" env -i A=1 "$SX" echo x -n)" "x -n"

echo "test_opt_order: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
