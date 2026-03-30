#!/bin/sh
# gnu_compare.sh — GNU-01: Compare matchbox builtin output+exit codes vs GNU tools
# Run representative args for each builtin, diff output against system tool.

MATCHBOX="${MATCHBOX:-$(dirname "$0")/../../build/bin/matchbox}"
PASS=0; FAIL=0

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# Create test data
printf 'banana\napple\ncherry\napple\ndate\n' > "$T/words.txt"
printf '3\n1\n4\n1\n5\n9\n2\n6\n' > "$T/nums.txt"
printf 'hello world\ngoodbye world\nhello there\n' > "$T/grep.txt"
printf 'one:two:three\nfour:five:six\n' > "$T/colons.txt"
printf 'aabbcc\nabc\n' > "$T/dup.txt"
mkdir -p "$T/dir/sub"
touch "$T/dir/a.c" "$T/dir/b.txt" "$T/dir/sub/c.c"
printf 'hello\n' > "$T/single.txt"
cp "$T/words.txt" "$T/src.txt"

check() {
    local desc="$1" mb_out="$2" sys_out="$3" mb_rc="$4" sys_rc="$5"
    if [ "$mb_out" = "$sys_out" ] && [ "$mb_rc" = "$sys_rc" ]; then
        printf 'PASS: %s\n' "$desc"
        PASS=$((PASS+1))
    else
        printf 'FAIL: %s\n' "$desc"
        if [ "$mb_out" != "$sys_out" ]; then
            printf '  sys: %s\n  mb:  %s\n' "$sys_out" "$mb_out"
        fi
        if [ "$mb_rc" != "$sys_rc" ]; then
            printf '  sys rc: %s  mb rc: %s\n' "$sys_rc" "$mb_rc"
        fi
        FAIL=$((FAIL+1))
    fi
}

cmp_cmd() {
    local desc="$1"; shift
    local mb_cmd="$1"; shift
    local sys_cmd="$1"; shift
    local mb_out sys_out mb_rc sys_rc
    mb_out=$("$MATCHBOX" $mb_cmd "$@" 2>&1); mb_rc=$?
    sys_out=$($sys_cmd "$@" 2>&1); sys_rc=$?
    check "$desc" "$mb_out" "$sys_out" "$mb_rc" "$sys_rc"
}

echo "--- sort ---"
cmp_cmd "sort default"         sort sort          "$T/words.txt"
cmp_cmd "sort -r"              sort sort -r       "$T/words.txt"
cmp_cmd "sort -u"              sort sort -u       "$T/words.txt"
cmp_cmd "sort -n"              sort sort -n       "$T/nums.txt"

echo "--- grep ---"
cmp_cmd "grep basic"           grep grep          "hello" "$T/grep.txt"
cmp_cmd "grep -c"              grep grep -c       "hello" "$T/grep.txt"
cmp_cmd "grep -i"              grep grep -i       "HELLO" "$T/grep.txt"
cmp_cmd "grep -v"              grep grep -v       "hello" "$T/grep.txt"
cmp_cmd "grep -n"              grep grep -n       "hello" "$T/grep.txt"
cmp_cmd "grep -q match exit"   grep grep -q       "hello" "$T/grep.txt"
cmp_cmd "grep -q nomatch exit" grep grep -q       "NOMATCH" "$T/grep.txt"
cmp_cmd "grep -F"              grep grep -F       "hello" "$T/grep.txt"

echo "--- wc ---"
cmp_cmd "wc -l"                wc wc -l          "$T/words.txt"
cmp_cmd "wc -w"                wc wc -w          "$T/words.txt"
cmp_cmd "wc -c"                wc wc -c          "$T/words.txt"

echo "--- sed ---"
cmp_cmd "sed s///"             sed sed            's/hello/hi/' "$T/grep.txt"
cmp_cmd "sed s///g"            sed sed            's/o/0/g' "$T/grep.txt"
cmp_cmd "sed d"                sed sed            '/goodbye/d' "$T/grep.txt"
cmp_cmd "sed -n p"             sed sed            -n '/hello/p' "$T/grep.txt"

echo "--- head/tail ---"
cmp_cmd "head -n 3"            head head -n 3    "$T/words.txt"
cmp_cmd "head default"         head head          "$T/words.txt"
cmp_cmd "tail -n 3"            tail tail -n 3    "$T/words.txt"
cmp_cmd "tail default"         tail tail          "$T/words.txt"

echo "--- cut ---"
cmp_cmd "cut -d: -f1"          cut cut -d: -f1   "$T/colons.txt"
cmp_cmd "cut -d: -f2"          cut cut -d: -f2   "$T/colons.txt"
cmp_cmd "cut -c1-3"            cut cut -c1-3     "$T/words.txt"

echo "--- tr ---"
check "tr translate" \
    "$(printf 'hello\n' | "$MATCHBOX" tr a-z A-Z)" \
    "$(printf 'hello\n' | tr a-z A-Z)" "0" "0"
check "tr -d" \
    "$(printf 'hello\n' | "$MATCHBOX" tr -d 'l')" \
    "$(printf 'hello\n' | tr -d 'l')" "0" "0"
check "tr -s" \
    "$(printf 'aabbcc\n' | "$MATCHBOX" tr -s 'a-c')" \
    "$(printf 'aabbcc\n' | tr -s 'a-c')" "0" "0"

echo "--- basename/dirname ---"
cmp_cmd "basename path"        basename basename  "/path/to/file.txt"
cmp_cmd "basename with suffix" basename basename  "/path/to/file.txt" ".txt"
cmp_cmd "dirname"              dirname dirname    "/path/to/file.txt"
cmp_cmd "dirname root"         dirname dirname    "/"

echo "--- find ---"
# Sort output for determinism
check "find -name *.c" \
    "$("$MATCHBOX" find "$T/dir" -name '*.c' 2>/dev/null | sort)" \
    "$(find "$T/dir" -name '*.c' 2>/dev/null | sort)" "0" "0"
check "find -type f" \
    "$("$MATCHBOX" find "$T/dir" -type f 2>/dev/null | sort)" \
    "$(find "$T/dir" -type f 2>/dev/null | sort)" "0" "0"
check "find -type d" \
    "$("$MATCHBOX" find "$T/dir" -type d 2>/dev/null | sort)" \
    "$(find "$T/dir" -type d 2>/dev/null | sort)" "0" "0"

echo "--- cat ---"
cmp_cmd "cat single file"      cat cat           "$T/single.txt"
cmp_cmd "cat multiple files"   cat cat           "$T/single.txt" "$T/single.txt"

echo "--- cp (exit code tests) ---"
"$MATCHBOX" cp /nonexistent_src /tmp/dest_mb 2>/dev/null; mb_rc=$?
cp /nonexistent_src /tmp/dest_sys 2>/dev/null; sys_rc=$?
check "cp nonexistent exit code" "" "" "$mb_rc" "$sys_rc"

echo "--- mkdir (exit code tests) ---"
"$MATCHBOX" mkdir /proc/impossible_mb 2>/dev/null; mb_rc=$?
mkdir /proc/impossible_sys 2>/dev/null; sys_rc=$?
check "mkdir impossible exit code" "" "" "$mb_rc" "$sys_rc"

echo "--- env ---"
# env no args: both print environment — just check it has PATH=
check "env has PATH" \
    "$("$MATCHBOX" env | grep '^PATH=' | head -1 | cut -c1-5)" \
    "$(env | grep '^PATH=' | head -1 | cut -c1-5)" "0" "0"

echo "--- stat (exit codes) ---"
"$MATCHBOX" stat /nonexistent_stat 2>/dev/null; mb_rc=$?
stat /nonexistent_stat 2>/dev/null; sys_rc=$?
check "stat nonexistent exit code" "" "" "$mb_rc" "$sys_rc"

printf '\nGNU comparison: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
