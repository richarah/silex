#!/bin/bash
# tests/compat/run.sh — TAP-format compatibility test runner
# Compares matchbox output against GNU coreutils
# chmod +x tests/compat/run.sh
#
# Usage: ./run.sh [MATCHBOX_BINARY] [TOOL]
#   MATCHBOX_BINARY  path to matchbox binary (default: build/bin/matchbox)
#   TOOL             only run tests for this tool (e.g., echo, cp, mkdir)
#
# Output: TAP format (https://testanything.org/)

set -u

MATCHBOX="${1:-build/bin/matchbox}"
TOOL="${2:-}"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# Temporary workspace
TMPDIR_COMPAT=$(mktemp -d)
trap 'rm -rf "$TMPDIR_COMPAT"' EXIT INT TERM

if [ ! -x "$MATCHBOX" ]; then
    echo "Bail out! matchbox binary not found or not executable: $MATCHBOX"
    exit 1
fi

# ---------------------------------------------------------------------------
# Core helpers
# ---------------------------------------------------------------------------

run_test() {
    local desc="$1"
    local mb_cmd="$2"       # command string passed to matchbox (no binary prefix)
    local ref_cmd="$3"      # full reference command string (using system tools)
    local tool_name="${4:-}"

    # Filter by tool if requested
    if [ -n "$TOOL" ] && [ -n "$tool_name" ] && [ "$tool_name" != "$TOOL" ]; then
        return
    fi

    TOTAL=$((TOTAL + 1))

    local got expected exit_got exit_expected
    got=$(eval "$MATCHBOX $mb_cmd" 2>&1)
    exit_got=$?
    expected=$(eval "$ref_cmd" 2>&1)
    exit_expected=$?

    if [ "$got" = "$expected" ] && [ "$exit_got" = "$exit_expected" ]; then
        echo "ok $TOTAL - $desc"
        PASS=$((PASS + 1))
    else
        echo "not ok $TOTAL - $desc"
        if [ "$exit_got" != "$exit_expected" ]; then
            echo "  # Expected exit=$exit_expected, got exit=$exit_got"
        fi
        if [ "$got" != "$expected" ]; then
            echo "  # Expected output: $(printf '%s' "$expected" | head -3 | cat -A)"
            echo "  # Got output:      $(printf '%s' "$got"      | head -3 | cat -A)"
        fi
        FAIL=$((FAIL + 1))
    fi
}

skip_test() {
    local desc="$1"
    TOTAL=$((TOTAL + 1))
    SKIP=$((SKIP + 1))
    echo "ok $TOTAL - $desc # SKIP"
}

# ---------------------------------------------------------------------------
# Setup test files
# ---------------------------------------------------------------------------

T="$TMPDIR_COMPAT"
printf 'hello world\n' > "$T/hello.txt"
printf 'line1\nline2\nline3\n' > "$T/lines.txt"
printf 'banana\napple\ncherry\ndate\nelderberry\n' > "$T/fruits.txt"
printf 'aaa\nbbb\nccc\naaa\n' > "$T/dupes.txt"
printf '  leading spaces\nno leading\n\tleading tab\n' > "$T/spaces.txt"
printf '3\n1\n4\n1\n5\n9\n2\n6\n' > "$T/numbers.txt"
printf 'foo bar baz\n' > "$T/words.txt"
printf '' > "$T/empty.txt"
# File with no trailing newline
printf 'no newline' > "$T/nonewline.txt"
# Multi-line with counts
printf 'alpha beta\ngamma delta epsilon\nzeta\n' > "$T/multiword.txt"
mkdir -p "$T/subdir"
printf 'subfile\n' > "$T/subdir/sub.txt"

# ---------------------------------------------------------------------------
# echo tests
# ---------------------------------------------------------------------------

run_test "echo: simple string"       "echo hello"                    "echo hello"              "echo"
run_test "echo: multiple args"       "echo hello world"              "echo hello world"        "echo"
run_test "echo: empty"               "echo"                          "echo"                    "echo"
run_test "echo: -n no newline"       "echo -n hello"                 "echo -n hello"           "echo"
run_test "echo: -e escape newline"   "echo -e 'a\\nb'"              "echo -e 'a\\nb'"         "echo"
run_test "echo: -e escape tab"       "echo -e 'a\\tb'"              "echo -e 'a\\tb'"         "echo"
run_test "echo: special chars"       "echo 'hello \$world'"          "echo 'hello \$world'"    "echo"

# ---------------------------------------------------------------------------
# mkdir tests
# ---------------------------------------------------------------------------

run_test "mkdir: create single dir" \
    "mkdir $T/compat_mk1" \
    "mkdir $T/ref_mk1" \
    "mkdir"

run_test "mkdir: -p nested" \
    "mkdir -p $T/compat_mk2/a/b/c" \
    "mkdir -p $T/ref_mk2/a/b/c" \
    "mkdir"

run_test "mkdir: -p existing (no error)" \
    "mkdir -p $T/compat_mk2" \
    "mkdir -p $T/ref_mk2" \
    "mkdir"

# Exit code for mkdir on existing dir without -p
run_test "mkdir: existing dir without -p exits nonzero" \
    "mkdir $T/compat_mk2" \
    "mkdir $T/ref_mk2" \
    "mkdir"

# ---------------------------------------------------------------------------
# cp tests
# ---------------------------------------------------------------------------

run_test "cp: copy file" \
    "cp $T/hello.txt $T/compat_cp1.txt" \
    "cp $T/hello.txt $T/ref_cp1.txt" \
    "cp"

run_test "cp: overwrite destination" \
    "cp $T/lines.txt $T/compat_cp1.txt" \
    "cp $T/lines.txt $T/ref_cp1.txt" \
    "cp"

run_test "cp: nonexistent source exits nonzero" \
    "cp $T/nosuchfile.txt $T/compat_cp2.txt" \
    "cp $T/nosuchfile.txt $T/ref_cp2.txt" \
    "cp"

run_test "cp: -r recursive directory" \
    "cp -r $T/subdir $T/compat_cpdir" \
    "cp -r $T/subdir $T/ref_cpdir" \
    "cp"

# ---------------------------------------------------------------------------
# cat tests
# ---------------------------------------------------------------------------

run_test "cat: single file"           "cat $T/hello.txt"             "cat $T/hello.txt"          "cat"
run_test "cat: multiple files"        "cat $T/hello.txt $T/words.txt" "cat $T/hello.txt $T/words.txt" "cat"
run_test "cat: empty file"            "cat $T/empty.txt"             "cat $T/empty.txt"          "cat"
run_test "cat: nonexistent exits 1"   "cat $T/nosuchfile.txt"        "cat $T/nosuchfile.txt"     "cat"
run_test "cat: -n number lines"       "cat -n $T/lines.txt"          "cat -n $T/lines.txt"       "cat"
run_test "cat: file with no newline"  "cat $T/nonewline.txt"         "cat $T/nonewline.txt"      "cat"

# ---------------------------------------------------------------------------
# wc tests
# ---------------------------------------------------------------------------

run_test "wc: -l line count"   "wc -l $T/lines.txt"     "wc -l $T/lines.txt"    "wc"
run_test "wc: -w word count"   "wc -w $T/words.txt"     "wc -w $T/words.txt"    "wc"
run_test "wc: -c byte count"   "wc -c $T/hello.txt"     "wc -c $T/hello.txt"    "wc"
run_test "wc: no flags (all)"  "wc $T/multiword.txt"    "wc $T/multiword.txt"   "wc"
run_test "wc: multiple files"  "wc -l $T/lines.txt $T/fruits.txt" \
                               "wc -l $T/lines.txt $T/fruits.txt" "wc"
run_test "wc: empty file"      "wc -l $T/empty.txt"     "wc -l $T/empty.txt"   "wc"

# ---------------------------------------------------------------------------
# sort tests
# ---------------------------------------------------------------------------

run_test "sort: default sort"         "sort $T/fruits.txt"           "sort $T/fruits.txt"         "sort"
run_test "sort: -r reverse"           "sort -r $T/fruits.txt"        "sort -r $T/fruits.txt"      "sort"
run_test "sort: -n numeric"           "sort -n $T/numbers.txt"       "sort -n $T/numbers.txt"     "sort"
run_test "sort: -u unique"            "sort -u $T/dupes.txt"         "sort -u $T/dupes.txt"       "sort"
run_test "sort: -rn reverse numeric"  "sort -rn $T/numbers.txt"      "sort -rn $T/numbers.txt"    "sort"
run_test "sort: empty file"           "sort $T/empty.txt"            "sort $T/empty.txt"          "sort"

# ---------------------------------------------------------------------------
# grep tests
# ---------------------------------------------------------------------------

run_test "grep: basic match"          "grep apple $T/fruits.txt"     "grep apple $T/fruits.txt"    "grep"
run_test "grep: no match exits 1"     "grep nomatch $T/fruits.txt"   "grep nomatch $T/fruits.txt"  "grep"
run_test "grep: -i case insensitive"  "grep -i APPLE $T/fruits.txt"  "grep -i APPLE $T/fruits.txt" "grep"
run_test "grep: -n line numbers"      "grep -n a $T/fruits.txt"      "grep -n a $T/fruits.txt"     "grep"
run_test "grep: -c count"             "grep -c a $T/fruits.txt"      "grep -c a $T/fruits.txt"     "grep"
run_test "grep: -v invert"            "grep -v a $T/fruits.txt"      "grep -v a $T/fruits.txt"     "grep"
run_test "grep: regex"                "grep 'a.*e' $T/fruits.txt"    "grep 'a.*e' $T/fruits.txt"   "grep"
run_test "grep: empty file"           "grep anything $T/empty.txt"   "grep anything $T/empty.txt"  "grep"

# ---------------------------------------------------------------------------
# sed tests
# ---------------------------------------------------------------------------

run_test "sed: substitution s/a/A/"      "sed 's/a/A/' $T/fruits.txt"    "sed 's/a/A/' $T/fruits.txt"    "sed"
run_test "sed: global substitution s/a/A/g" "sed 's/a/A/g' $T/fruits.txt" "sed 's/a/A/g' $T/fruits.txt" "sed"
run_test "sed: delete matching line /d"  "sed '/apple/d' $T/fruits.txt"  "sed '/apple/d' $T/fruits.txt"  "sed"
run_test "sed: print matching /p"        "sed -n '/apple/p' $T/fruits.txt" "sed -n '/apple/p' $T/fruits.txt" "sed"
run_test "sed: empty file"               "sed 's/a/A/' $T/empty.txt"     "sed 's/a/A/' $T/empty.txt"     "sed"

# ---------------------------------------------------------------------------
# basename / dirname tests
# ---------------------------------------------------------------------------

run_test "basename: strip dir"           "basename /usr/local/bin"       "/usr/bin/basename /usr/local/bin"  "basename"
run_test "basename: strip suffix"        "basename file.txt .txt"        "/usr/bin/basename file.txt .txt"   "basename"
run_test "basename: just filename"       "basename hello.sh"             "/usr/bin/basename hello.sh"        "basename"
run_test "basename: trailing slash"      "basename /usr/local/"          "/usr/bin/basename /usr/local/"     "basename"

run_test "dirname: strip base"           "dirname /usr/local/bin"        "/usr/bin/dirname /usr/local/bin"   "dirname"
run_test "dirname: single component"     "dirname file.txt"              "/usr/bin/dirname file.txt"         "dirname"
run_test "dirname: root"                 "dirname /"                     "/usr/bin/dirname /"                "dirname"
run_test "dirname: trailing slash"       "dirname /usr/local/"           "/usr/bin/dirname /usr/local/"      "dirname"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "1..$TOTAL"
echo "# Results: $PASS passed, $FAIL failed, $SKIP skipped out of $TOTAL"
[ "$FAIL" -eq 0 ]
