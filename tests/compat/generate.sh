#!/bin/bash
# tests/compat/generate.sh — generate TAP test cases for each builtin
# chmod +x tests/compat/generate.sh
#
# Usage: ./generate.sh [output_dir]
#   output_dir  where to write generated test files (default: tests/compat/generated)
#
# For each builtin, creates test inputs of various sizes and generates
# TAP-format test cases comparing silex output against system coreutils.

set -euo pipefail

SILEX="${SILEX:-build/bin/silex}"
OUTDIR="${1:-tests/compat/generated}"
TMPDIR_GEN=$(mktemp -d)
trap 'rm -rf "$TMPDIR_GEN"' EXIT INT TERM

mkdir -p "$OUTDIR"

# ---------------------------------------------------------------------------
# Test file generation helpers
# ---------------------------------------------------------------------------

# Create files of multiple sizes for content-processing tools
create_test_files() {
    local dir="$1"
    mkdir -p "$dir"

    # Small: 10 lines
    seq 1 10 > "$dir/small.txt"

    # Medium: 100 lines with words
    for i in $(seq 1 100); do
        printf 'line%d alpha beta gamma delta epsilon zeta eta theta iota\n' "$i"
    done > "$dir/medium.txt"

    # Large: 1000 lines
    seq 1 1000 > "$dir/large.txt"

    # Binary-safe: mixed content
    printf 'aaa\nbbb\nccc\naaa\nbbb\n' > "$dir/dupes.txt"

    # Sorted input
    printf 'apple\nbanana\ncherry\ndate\n' > "$dir/sorted.txt"

    # Reverse-sorted input
    printf 'date\ncherry\nbanana\napple\n' > "$dir/rsorted.txt"

    # Numeric
    printf '10\n2\n30\n1\n20\n3\n' > "$dir/numeric.txt"

    # With spaces
    printf '  leading\nno leading\n\tleading tab\n' > "$dir/spaces.txt"

    # Empty
    > "$dir/empty.txt"

    # Single line, no newline
    printf 'no trailing newline' > "$dir/nonewline.txt"

    # CSV-like
    printf 'name,age,city\nalice,30,london\nbob,25,paris\ncarol,35,berlin\n' > "$dir/csv.txt"

    # Mixed case
    printf 'Hello World\nfoo BAR\nbaz Qux\n' > "$dir/mixed_case.txt"
}

TESTFILES="$TMPDIR_GEN/files"
create_test_files "$TESTFILES"

# ---------------------------------------------------------------------------
# TAP output helper
# ---------------------------------------------------------------------------

TOTAL=0
PASS=0
FAIL=0

tap_test() {
    local desc="$1"
    local mb_cmd="$2"
    local ref_cmd="$3"

    TOTAL=$((TOTAL + 1))
    local got expected exit_got exit_expected
    got=$(eval "$SILEX $mb_cmd" 2>&1)
    exit_got=$?
    expected=$(eval "$ref_cmd" 2>&1)
    exit_expected=$?

    if [ "$got" = "$expected" ] && [ "$exit_got" = "$exit_expected" ]; then
        printf 'ok %d - %s\n' "$TOTAL" "$desc"
        PASS=$((PASS + 1))
    else
        printf 'not ok %d - %s\n' "$TOTAL" "$desc"
        [ "$exit_got" != "$exit_expected" ] && \
            printf '  # exit: expected=%s got=%s\n' "$exit_expected" "$exit_got"
        [ "$got" != "$expected" ] && \
            printf '  # output differs (first line): expected=%s got=%s\n' \
                "$(printf '%s' "$expected" | head -1)" \
                "$(printf '%s' "$got"      | head -1)"
        FAIL=$((FAIL + 1))
    fi
}

# ===========================================================================
# echo
# ===========================================================================
echo "# --- echo ---"
tap_test "echo: no args"                 "echo"                  "echo"
tap_test "echo: simple word"             "echo hello"            "echo hello"
tap_test "echo: multiple words"          "echo hello world"      "echo hello world"
tap_test "echo: -n suppress newline"     "echo -n hello"         "echo -n hello"
tap_test "echo: -e newline escape"       "echo -e 'a\\nb'"       "echo -e 'a\\nb'"
tap_test "echo: -e tab escape"           "echo -e 'a\\tb'"       "echo -e 'a\\tb'"
tap_test "echo: quoted spaces"           "echo 'a b  c'"         "echo 'a b  c'"
tap_test "echo: empty string"            "echo ''"               "echo ''"
tap_test "echo: numeric"                 "echo 42"               "echo 42"
tap_test "echo: special chars"           "echo '!@#'"            "echo '!@#'"

# ===========================================================================
# cat
# ===========================================================================
echo "# --- cat ---"
tap_test "cat: small file"              "cat $TESTFILES/small.txt"      "cat $TESTFILES/small.txt"
tap_test "cat: medium file"             "cat $TESTFILES/medium.txt"     "cat $TESTFILES/medium.txt"
tap_test "cat: large file"              "cat $TESTFILES/large.txt"      "cat $TESTFILES/large.txt"
tap_test "cat: empty file"              "cat $TESTFILES/empty.txt"      "cat $TESTFILES/empty.txt"
tap_test "cat: no trailing newline"     "cat $TESTFILES/nonewline.txt"  "cat $TESTFILES/nonewline.txt"
tap_test "cat: -n number lines small"   "cat -n $TESTFILES/small.txt"   "cat -n $TESTFILES/small.txt"
tap_test "cat: multiple files"          "cat $TESTFILES/small.txt $TESTFILES/dupes.txt" \
                                        "cat $TESTFILES/small.txt $TESTFILES/dupes.txt"
tap_test "cat: nonexistent file"        "cat $TESTFILES/nosuchfile.txt" "cat $TESTFILES/nosuchfile.txt"
tap_test "cat: -A show nonprinting"     "cat -A $TESTFILES/spaces.txt"  "cat -A $TESTFILES/spaces.txt"

# ===========================================================================
# wc
# ===========================================================================
echo "# --- wc ---"
tap_test "wc -l: small"     "wc -l $TESTFILES/small.txt"   "wc -l $TESTFILES/small.txt"
tap_test "wc -l: medium"    "wc -l $TESTFILES/medium.txt"  "wc -l $TESTFILES/medium.txt"
tap_test "wc -w: medium"    "wc -w $TESTFILES/medium.txt"  "wc -w $TESTFILES/medium.txt"
tap_test "wc -c: small"     "wc -c $TESTFILES/small.txt"   "wc -c $TESTFILES/small.txt"
tap_test "wc: all (no flags)" "wc $TESTFILES/medium.txt"   "wc $TESTFILES/medium.txt"
tap_test "wc -l: empty"     "wc -l $TESTFILES/empty.txt"   "wc -l $TESTFILES/empty.txt"
tap_test "wc -l: no newline" "wc -l $TESTFILES/nonewline.txt" "wc -l $TESTFILES/nonewline.txt"
tap_test "wc: multiple files" "wc -l $TESTFILES/small.txt $TESTFILES/dupes.txt" \
                              "wc -l $TESTFILES/small.txt $TESTFILES/dupes.txt"

# ===========================================================================
# sort
# ===========================================================================
echo "# --- sort ---"
tap_test "sort: default"        "sort $TESTFILES/sorted.txt"    "sort $TESTFILES/sorted.txt"
tap_test "sort: reverse"        "sort -r $TESTFILES/sorted.txt" "sort -r $TESTFILES/sorted.txt"
tap_test "sort: numeric"        "sort -n $TESTFILES/numeric.txt" "sort -n $TESTFILES/numeric.txt"
tap_test "sort: unique"         "sort -u $TESTFILES/dupes.txt"  "sort -u $TESTFILES/dupes.txt"
tap_test "sort: reverse numeric" "sort -rn $TESTFILES/numeric.txt" "sort -rn $TESTFILES/numeric.txt"
tap_test "sort: already sorted"  "sort $TESTFILES/sorted.txt"   "sort $TESTFILES/sorted.txt"
tap_test "sort: empty file"      "sort $TESTFILES/empty.txt"    "sort $TESTFILES/empty.txt"
tap_test "sort: large file"      "sort $TESTFILES/large.txt"    "sort $TESTFILES/large.txt"

# ===========================================================================
# grep
# ===========================================================================
echo "# --- grep ---"
tap_test "grep: literal match"         "grep 'alpha' $TESTFILES/medium.txt"   "grep 'alpha' $TESTFILES/medium.txt"
tap_test "grep: no match"              "grep 'ZZZNOMATCH' $TESTFILES/medium.txt" "grep 'ZZZNOMATCH' $TESTFILES/medium.txt"
tap_test "grep: -i case insensitive"   "grep -i 'ALPHA' $TESTFILES/medium.txt" "grep -i 'ALPHA' $TESTFILES/medium.txt"
tap_test "grep: -n line numbers"       "grep -n 'alpha' $TESTFILES/medium.txt" "grep -n 'alpha' $TESTFILES/medium.txt"
tap_test "grep: -c count matches"      "grep -c 'alpha' $TESTFILES/medium.txt" "grep -c 'alpha' $TESTFILES/medium.txt"
tap_test "grep: -v invert match"       "grep -v 'alpha' $TESTFILES/medium.txt | head -5" \
                                       "grep -v 'alpha' $TESTFILES/medium.txt | head -5"
tap_test "grep: regex wildcard"        "grep 'line[0-9]' $TESTFILES/medium.txt | head -5" \
                                       "grep 'line[0-9]' $TESTFILES/medium.txt | head -5"
tap_test "grep: -l files with matches" "grep -l 'alpha' $TESTFILES/medium.txt $TESTFILES/small.txt" \
                                       "grep -l 'alpha' $TESTFILES/medium.txt $TESTFILES/small.txt"
tap_test "grep: empty file"            "grep 'anything' $TESTFILES/empty.txt" \
                                       "grep 'anything' $TESTFILES/empty.txt"

# ===========================================================================
# sed
# ===========================================================================
echo "# --- sed ---"
tap_test "sed: s/a/A/ first"           "sed 's/a/A/' $TESTFILES/sorted.txt"    "sed 's/a/A/' $TESTFILES/sorted.txt"
tap_test "sed: s/a/A/g global"         "sed 's/a/A/g' $TESTFILES/sorted.txt"   "sed 's/a/A/g' $TESTFILES/sorted.txt"
tap_test "sed: /pattern/d delete"      "sed '/apple/d' $TESTFILES/sorted.txt"  "sed '/apple/d' $TESTFILES/sorted.txt"
tap_test "sed: -n /p print match"      "sed -n '/apple/p' $TESTFILES/sorted.txt" "sed -n '/apple/p' $TESTFILES/sorted.txt"
tap_test "sed: empty file"             "sed 's/a/A/' $TESTFILES/empty.txt"     "sed 's/a/A/' $TESTFILES/empty.txt"
tap_test "sed: addr range 2,3p"        "sed -n '2,3p' $TESTFILES/sorted.txt"   "sed -n '2,3p' $TESTFILES/sorted.txt"
tap_test "sed: =  print line numbers"  "sed '=' $TESTFILES/sorted.txt"         "sed '=' $TESTFILES/sorted.txt"

# ===========================================================================
# basename / dirname
# ===========================================================================
echo "# --- basename/dirname ---"
tap_test "basename: simple path"    "basename /usr/local/bin"       "/usr/bin/basename /usr/local/bin"
tap_test "basename: with suffix"    "basename file.txt .txt"        "/usr/bin/basename file.txt .txt"
tap_test "basename: trailing slash" "basename /usr/local/"          "/usr/bin/basename /usr/local/"
tap_test "basename: root"           "basename /"                    "/usr/bin/basename /"
tap_test "basename: no dir"         "basename file.sh"              "/usr/bin/basename file.sh"
tap_test "dirname: simple"          "dirname /usr/local/bin"        "/usr/bin/dirname /usr/local/bin"
tap_test "dirname: no dir part"     "dirname file.txt"              "/usr/bin/dirname file.txt"
tap_test "dirname: trailing slash"  "dirname /usr/local/"           "/usr/bin/dirname /usr/local/"
tap_test "dirname: root"            "dirname /"                     "/usr/bin/dirname /"
tap_test "dirname: double slash"    "dirname //usr/local"           "/usr/bin/dirname //usr/local"

# ===========================================================================
# mkdir
# ===========================================================================
echo "# --- mkdir ---"
MKBASE="$TMPDIR_GEN/mkdir_tests"
mkdir -p "$MKBASE"
tap_test "mkdir: simple dir"        "mkdir $MKBASE/mb1"             "mkdir $MKBASE/ref1"
tap_test "mkdir: -p nested"         "mkdir -p $MKBASE/mb2/a/b"      "mkdir -p $MKBASE/ref2/a/b"
tap_test "mkdir: -p existing ok"    "mkdir -p $MKBASE/mb2"          "mkdir -p $MKBASE/ref2"
tap_test "mkdir: existing fails"    "mkdir $MKBASE/mb2"             "mkdir $MKBASE/ref2"

# ===========================================================================
# cp
# ===========================================================================
echo "# --- cp ---"
CPBASE="$TMPDIR_GEN/cp_tests"
mkdir -p "$CPBASE"
tap_test "cp: simple copy"          "cp $TESTFILES/small.txt $CPBASE/mb_cp1.txt" \
                                    "cp $TESTFILES/small.txt $CPBASE/ref_cp1.txt"
tap_test "cp: overwrite"            "cp $TESTFILES/dupes.txt $CPBASE/mb_cp1.txt" \
                                    "cp $TESTFILES/dupes.txt $CPBASE/ref_cp1.txt"
tap_test "cp: nonexistent src"      "cp $TESTFILES/nosuch.txt $CPBASE/mb_cp2.txt" \
                                    "cp $TESTFILES/nosuch.txt $CPBASE/ref_cp2.txt"
tap_test "cp: -r recursive"         "cp -r $TESTFILES $CPBASE/mb_cpdir" \
                                    "cp -r $TESTFILES $CPBASE/ref_cpdir"

# ===========================================================================
# Summary
# ===========================================================================
echo ""
echo "1..$TOTAL"
echo "# Results: $PASS passed, $FAIL failed out of $TOTAL"
[ "$FAIL" -eq 0 ]
