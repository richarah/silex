#!/bin/sh
# new_flags_test.sh — Test newly implemented GNU flags from FLAG_GAPS.md
# Tests critical path flags and additional GNU compatibility flags.

SILEX="${SILEX:-$(dirname "$0")/../../build/bin/silex}"
PASS=0; FAIL=0

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# Create test data
printf 'line1\nline2\nline3\nline4\nline5\nline6\nline7\n' > "$T/lines.txt"
printf 'match1\nnomatch\nmatch2\nnomatch\nmatch3\n' > "$T/match.txt"
printf 'apple\nbanana\ncherry\ndate\n' > "$T/words.txt"
printf '#!/bin/sh\necho test\n' > "$T/script.sed"
printf 's/line/LINE/\n' > "$T/simple.sed"
mkdir -p "$T/findtest/sub1/sub2"
touch "$T/findtest/file1.txt"
touch "$T/findtest/sub1/file2.txt"
touch "$T/findtest/sub1/sub2/file3.txt"
echo "arg1" > "$T/args.txt"
echo "arg2" >> "$T/args.txt"
echo "arg3" >> "$T/args.txt"

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
    mb_out=$("$SILEX" $mb_cmd "$@" 2>&1); mb_rc=$?
    sys_out=$($sys_cmd "$@" 2>&1); sys_rc=$?
    check "$desc" "$mb_out" "$sys_out" "$mb_rc" "$sys_rc"
}

echo "=== CRITICAL PATH FLAGS ==="

echo "--- grep context flags (-A/-B/-C) ---"
cmp_cmd "grep -A 1 (after-context)"  grep grep -A 1 "match1" "$T/match.txt"
cmp_cmd "grep -B 1 (before-context)" grep grep -B 1 "match2" "$T/match.txt"
cmp_cmd "grep -C 1 (context)"        grep grep -C 1 "match2" "$T/match.txt"
cmp_cmd "grep -A 2 (2 lines after)"  grep grep -A 2 "match1" "$T/match.txt"

echo "--- grep -o (only-matching) ---"
cmp_cmd "grep -o pattern"            grep grep -o "match[0-9]" "$T/match.txt"

echo "--- grep -m N (max-count) ---"
cmp_cmd "grep -m 2 (stop after 2)"   grep grep -m 2 "match" "$T/match.txt"

echo "--- grep -L (files-without-match) ---"
# grep -L prints filenames of files WITHOUT match
check "grep -L (files without match)" \
    "$("$SILEX" grep -L "nomatch" "$T/words.txt" 2>&1)" \
    "$(grep -L "nomatch" "$T/words.txt" 2>&1)" "0" "0"

echo "--- grep -H (always print filename) ---"
cmp_cmd "grep -H (with filename)"    grep grep -H "line1" "$T/lines.txt"

echo "--- grep -h (never print filename) ---"
cmp_cmd "grep -h (no filename)"      grep grep -h "line1" "$T/lines.txt"

echo "--- grep -x (match whole line) ---"
cmp_cmd "grep -x (exact line match)" grep grep -x "match1" "$T/match.txt"

echo "--- find -print0 (NUL-terminated) ---"
# Critical: used with xargs -0
check "find -print0 output" \
    "$("$SILEX" find "$T/findtest" -name "*.txt" -print0 2>/dev/null | od -An -tx1 | head -1)" \
    "$(find "$T/findtest" -name "*.txt" -print0 2>/dev/null | od -An -tx1 | head -1)" "0" "0"

echo "--- xargs -L N (max-lines) ---"
check "xargs -L 1 (one line per invocation)" \
    "$(printf 'a\nb\nc\n' | "$SILEX" xargs -L 1 echo)" \
    "$(printf 'a\nb\nc\n' | xargs -L 1 echo)" "0" "0"

check "xargs -L 2 (two lines per invocation)" \
    "$(printf 'a\nb\nc\nd\n' | "$SILEX" xargs -L 2 echo)" \
    "$(printf 'a\nb\nc\nd\n' | xargs -L 2 echo)" "0" "0"

echo "--- sed -f FILE (script from file) ---"
cmp_cmd "sed -f simple script"       sed sed -f "$T/simple.sed" "$T/lines.txt"

echo "--- install -D (create leading directories) ---"
# install -D should create parent dirs if they don't exist
rm -rf "$T/install_test"
"$SILEX" install -D "$T/lines.txt" "$T/install_test/subdir/file.txt" 2>/dev/null; mb_rc=$?
[ -f "$T/install_test/subdir/file.txt" ]; mb_exist=$?

rm -rf "$T/install_test_sys"
install -D "$T/lines.txt" "$T/install_test_sys/subdir/file.txt" 2>/dev/null; sys_rc=$?
[ -f "$T/install_test_sys/subdir/file.txt" ]; sys_exist=$?

check "install -D creates dirs" "" "" "$mb_exist" "$sys_exist"
check "install -D exit code" "" "" "$mb_rc" "$sys_rc"

echo ""
echo "=== ADDITIONAL GREP FLAGS ==="

echo "--- grep -b (byte-offset) ---"
cmp_cmd "grep -b (show byte offset)" grep grep -b "line3" "$T/lines.txt"

echo "--- grep -Z (NUL-terminated filenames) ---"
# When printing filenames, terminate with NUL instead of newline
check "grep -Z output format" \
    "$("$SILEX" grep -Z "line1" "$T/lines.txt" 2>/dev/null | od -An -tx1 | head -1)" \
    "$(grep -Z "line1" "$T/lines.txt" 2>/dev/null | od -An -tx1 | head -1)" "0" "0"

echo ""
echo "=== ADDITIONAL SORT FLAGS ==="

echo "--- sort -M (month sort) ---"
printf 'Dec\nJan\nFeb\n' > "$T/months.txt"
cmp_cmd "sort -M (month names)"      sort sort -M "$T/months.txt"

echo "--- sort -c (check sorted) ---"
"$SILEX" sort -c "$T/words.txt" 2>/dev/null; mb_rc=$?
sort -c "$T/words.txt" 2>/dev/null; sys_rc=$?
check "sort -c (check sorted) exit code" "" "" "$mb_rc" "$sys_rc"

echo "--- sort -R (random sort) ---"
# Can't compare output (it's random), but verify it runs without error
"$SILEX" sort -R "$T/words.txt" >/dev/null 2>&1; mb_rc=$?
sort -R "$T/words.txt" >/dev/null 2>&1; sys_rc=$?
check "sort -R runs without error" "" "" "$mb_rc" "$sys_rc"

echo ""
echo "=== ADDITIONAL XARGS FLAGS ==="

echo "--- xargs -a FILE (read from file) ---"
check "xargs -a (read args from file)" \
    "$("$SILEX" xargs -a "$T/args.txt" echo)" \
    "$(xargs -a "$T/args.txt" echo)" "0" "0"

echo "--- xargs -s N (max command line) ---"
# -s limits command line length; verify it accepts the flag
printf 'a b c d e\n' | "$SILEX" xargs -s 100 echo >/dev/null 2>&1; mb_rc=$?
printf 'a b c d e\n' | xargs -s 100 echo >/dev/null 2>&1; sys_rc=$?
check "xargs -s accepts flag" "" "" "$mb_rc" "$sys_rc"

echo "--- xargs -t (trace commands) ---"
# -t prints command before executing; check stderr has output
mb_trace=$(printf 'test\n' | "$SILEX" xargs -t echo 2>&1 | grep -c "echo")
sys_trace=$(printf 'test\n' | xargs -t echo 2>&1 | grep -c "echo")
check "xargs -t traces commands" "" "" "$mb_trace" "$sys_trace"

echo ""
echo "=== ADDITIONAL INSTALL FLAGS ==="

echo "--- install -v (verbose) ---"
rm -f "$T/install_v_test"
mb_v=$("$SILEX" install -v "$T/lines.txt" "$T/install_v_test" 2>&1 | grep -c "'$T/install_v_test'")
rm -f "$T/install_v_sys"
sys_v=$(install -v "$T/lines.txt" "$T/install_v_sys" 2>&1 | grep -c "'$T/install_v_sys'")
check "install -v shows destination" "" "" "$mb_v" "$sys_v"

echo "--- install -t DIR (target directory) ---"
rm -rf "$T/install_t"
mkdir -p "$T/install_t"
"$SILEX" install -t "$T/install_t" "$T/lines.txt" 2>/dev/null; mb_rc=$?
[ -f "$T/install_t/lines.txt" ]; mb_exist=$?

rm -rf "$T/install_t_sys"
mkdir -p "$T/install_t_sys"
install -t "$T/install_t_sys" "$T/lines.txt" 2>/dev/null; sys_rc=$?
[ -f "$T/install_t_sys/lines.txt" ]; sys_exist=$?

check "install -t creates file in target" "" "" "$mb_exist" "$sys_exist"

echo ""
echo "=== ADDITIONAL FIND FLAGS ==="

echo "--- find -newer FILE ---"
touch -t 202301010000 "$T/old_file"
touch -t 202312310000 "$T/new_file"
check "find -newer (modification time)" \
    "$("$SILEX" find "$T" -name "*_file" -newer "$T/old_file" 2>/dev/null | sort)" \
    "$(find "$T" -name "*_file" -newer "$T/old_file" 2>/dev/null | sort)" "0" "0"

echo "--- find -size N ---"
dd if=/dev/zero of="$T/bigfile" bs=1024 count=5 2>/dev/null
check "find -size +1k" \
    "$("$SILEX" find "$T" -name "bigfile" -size +1k 2>/dev/null)" \
    "$(find "$T" -name "bigfile" -size +1k 2>/dev/null)" "0" "0"

echo ""
printf '\nNew flags test: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
