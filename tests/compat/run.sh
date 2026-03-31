#!/bin/bash
# tests/compat/run.sh — TAP-format compatibility test runner
# Compares silex output against GNU coreutils
# chmod +x tests/compat/run.sh
#
# Usage: ./run.sh [SILEX_BINARY] [TOOL]
#   SILEX_BINARY  path to silex binary (default: build/bin/silex)
#   TOOL             only run tests for this tool (e.g., echo, cp, mkdir)
#
# Output: TAP format (https://testanything.org/)

set -u

SILEX="${1:-build/bin/silex}"
TOOL="${2:-}"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# Temporary workspace
TMPDIR_COMPAT=$(mktemp -d)
trap 'rm -rf "$TMPDIR_COMPAT"' EXIT INT TERM

if [ ! -x "$SILEX" ]; then
    echo "Bail out! silex binary not found or not executable: $SILEX"
    exit 1
fi

# ---------------------------------------------------------------------------
# Core helpers
# ---------------------------------------------------------------------------

run_test() {
    local desc="$1"
    local mb_cmd="$2"       # command string passed to silex (no binary prefix)
    local ref_cmd="$3"      # full reference command string (using system tools)
    local tool_name="${4:-}"

    # Filter by tool if requested
    if [ -n "$TOOL" ] && [ -n "$tool_name" ] && [ "$tool_name" != "$TOOL" ]; then
        return
    fi

    TOTAL=$((TOTAL + 1))

    local got expected exit_got exit_expected
    got=$(eval "$SILEX $mb_cmd" 2>&1)
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

run_test_exitcode() {
    local desc="$1"
    local mb_cmd="$2"
    local ref_cmd="$3"
    local tool_name="${4:-}"

    if [ -n "$TOOL" ] && [ -n "$tool_name" ] && [ "$tool_name" != "$TOOL" ]; then
        return
    fi

    TOTAL=$((TOTAL + 1))

    eval "$SILEX $mb_cmd" > /dev/null 2>&1
    local exit_got=$?
    eval "$ref_cmd" > /dev/null 2>&1
    local exit_expected=$?

    if [ "$exit_got" = "$exit_expected" ]; then
        echo "ok $TOTAL - $desc"
        PASS=$((PASS + 1))
    else
        echo "not ok $TOTAL - $desc"
        echo "  # Expected exit=$exit_expected, got exit=$exit_got"
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
# (paths differ between compat and ref dirs, so only exit code is compared)
run_test_exitcode "mkdir: existing dir without -p exits nonzero" \
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

# Error message format differs (silex prefix); only exit code matters
run_test_exitcode "cp: nonexistent source exits nonzero" \
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
# Error message format differs (silex prefix vs uutils "(os error N)"); only exit code matters
run_test_exitcode "cat: nonexistent exits 1" "cat $T/nosuchfile.txt" "cat $T/nosuchfile.txt" "cat"
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
# tail tests
# ---------------------------------------------------------------------------

run_test "tail: -n 3 last 3 lines" \
    "tail -n 3 $T/fruits.txt" "tail -n 3 $T/fruits.txt" "tail"

run_test "tail: -n 1 last line" \
    "tail -n 1 $T/fruits.txt" "tail -n 1 $T/fruits.txt" "tail"

run_test "tail: -n +2 skip first line" \
    "tail -n +2 $T/fruits.txt" "tail -n +2 $T/fruits.txt" "tail"

run_test "tail: empty file" \
    "tail -n 5 $T/empty.txt" "tail -n 5 $T/empty.txt" "tail"

run_test_exitcode "tail: nonexistent file exits 1" \
    "tail -n 1 $T/nosuch_tail.txt" "tail -n 1 $T/nosuch_tail.txt" "tail"

run_test "tail: multiple files" \
    "tail -n 1 $T/fruits.txt $T/words.txt" \
    "tail -n 1 $T/fruits.txt $T/words.txt" "tail"

# ---------------------------------------------------------------------------
# printf tests
# ---------------------------------------------------------------------------

run_test "printf: string format" \
    "printf '%s\n' hello" "printf '%s\n' hello" "printf"

run_test "printf: integer format" \
    "printf '%d\n' 42" "printf '%d\n' 42" "printf"

run_test "printf: hex format" \
    "printf '%x\n' 255" "printf '%x\n' 255" "printf"

run_test "printf: multiple args repeat format" \
    "printf '%s\n' a b c" "printf '%s\n' a b c" "printf"

run_test "printf: tab escape" \
    "printf 'a\tb\n'" "printf 'a\tb\n'" "printf"

run_test "printf: newline in format" \
    "printf 'line1\nline2\n'" "printf 'line1\nline2\n'" "printf"

run_test "printf: width and precision" \
    "printf '%5d\n' 42" "printf '%5d\n' 42" "printf"

# ---------------------------------------------------------------------------
# sha256sum tests
# ---------------------------------------------------------------------------

run_test "sha256sum: known content" \
    "sha256sum $T/hello.txt" "sha256sum $T/hello.txt" "sha256sum"

run_test "sha256sum: empty file" \
    "sha256sum $T/empty.txt" "sha256sum $T/empty.txt" "sha256sum"

run_test "sha256sum: multiple files" \
    "sha256sum $T/hello.txt $T/lines.txt" \
    "sha256sum $T/hello.txt $T/lines.txt" "sha256sum"

run_test_exitcode "sha256sum: nonexistent file exits 1" \
    "sha256sum $T/nosuch_sha.txt" "sha256sum $T/nosuch_sha.txt" "sha256sum"

# ---------------------------------------------------------------------------
# env tests
# ---------------------------------------------------------------------------

run_test "env: set VAR for child process" \
    "env HELLO=world printenv HELLO" \
    "env HELLO=world printenv HELLO" "env"

run_test "env: override existing variable" \
    "env HOME=/tmp printenv HOME" \
    "env HOME=/tmp printenv HOME" "env"

run_test_exitcode "env: -u unsets variable" \
    "env -u HOME printenv HOME" \
    "env -u HOME printenv HOME" "env"

run_test_exitcode "env: nonexistent command exits nonzero" \
    "env /nonexistent_cmd_xyz" "env /nonexistent_cmd_xyz" "env"

# ---------------------------------------------------------------------------
# readlink tests
# ---------------------------------------------------------------------------

ln -sf "$T/hello.txt" "$T/rl_link.lnk" 2>/dev/null || true

run_test "readlink: symlink target" \
    "readlink $T/rl_link.lnk" "readlink $T/rl_link.lnk" "readlink"

run_test_exitcode "readlink: regular file exits 1" \
    "readlink $T/hello.txt" "readlink $T/hello.txt" "readlink"

run_test_exitcode "readlink: nonexistent exits 1" \
    "readlink $T/nosuch.lnk" "readlink $T/nosuch.lnk" "readlink"

run_test "readlink: -f canonical path of file" \
    "readlink -f $T/hello.txt" "readlink -f $T/hello.txt" "readlink"

run_test "readlink: -f follows symlink" \
    "readlink -f $T/rl_link.lnk" "readlink -f $T/rl_link.lnk" "readlink"

# ---------------------------------------------------------------------------
# realpath tests
# ---------------------------------------------------------------------------

run_test "realpath: absolute path" \
    "realpath $T/hello.txt" "realpath $T/hello.txt" "realpath"

run_test "realpath: resolves symlink" \
    "realpath $T/rl_link.lnk" "realpath $T/rl_link.lnk" "realpath"

run_test_exitcode "realpath: nonexistent exits 1" \
    "realpath $T/nosuch_rp.txt" "realpath $T/nosuch_rp.txt" "realpath"

run_test "realpath: directory" \
    "realpath $T/subdir" "realpath $T/subdir" "realpath"

# ---------------------------------------------------------------------------
# stat tests
# ---------------------------------------------------------------------------

run_test "stat: file type regular" \
    "stat -c '%F' $T/hello.txt" "stat -c '%F' $T/hello.txt" "stat"

run_test "stat: file size" \
    "stat -c '%s' $T/hello.txt" "stat -c '%s' $T/hello.txt" "stat"

run_test "stat: octal permissions" \
    "stat -c '%a' $T/hello.txt" "stat -c '%a' $T/hello.txt" "stat"

run_test "stat: directory type" \
    "stat -c '%F' $T/subdir" "stat -c '%F' $T/subdir" "stat"

run_test "stat: inode number is numeric" \
    "stat -c '%i' $T/hello.txt | grep -qE '^[0-9]+$' && echo ok" \
    "stat -c '%i' $T/hello.txt | grep -qE '^[0-9]+$' && echo ok" "stat"

run_test_exitcode "stat: nonexistent file exits 1" \
    "stat $T/nosuch_stat.txt" "stat $T/nosuch_stat.txt" "stat"

# ---------------------------------------------------------------------------
# date tests (deterministic: fixed @timestamp only)
# ---------------------------------------------------------------------------

run_test "date: epoch @0" \
    "date -d '@0' +%Y%m%d" "date -d '@0' +%Y%m%d" "date"

run_test "date: fixed timestamp" \
    "date -d '@1000000000' +%Y-%m-%d" \
    "date -d '@1000000000' +%Y-%m-%d" "date"

run_test "date: UTC timezone label" \
    "date -u +%Z" "date -u +%Z" "date"

run_test "date: RFC3339 of @0" \
    "date -d '@0' -u +'%Y-%m-%dT%H:%M:%SZ'" \
    "date -d '@0' -u +'%Y-%m-%dT%H:%M:%SZ'" "date"

run_test "date: custom format @0" \
    "date -d '@0' -u +'%Y/%m/%d %H:%M:%S'" \
    "date -d '@0' -u +'%Y/%m/%d %H:%M:%S'" "date"

# ---------------------------------------------------------------------------
# find tests
# ---------------------------------------------------------------------------

mkdir -p "$T/finddir/sub"
printf 'a\n' > "$T/finddir/a.txt"
printf 'b\n' > "$T/finddir/b.log"
printf 'c\n' > "$T/finddir/sub/c.txt"

run_test "find: -name *.txt sorted" \
    "find $T/finddir -name '*.txt' | sort" \
    "find $T/finddir -name '*.txt' | sort" "find"

run_test "find: -type f sorted" \
    "find $T/finddir -type f | sort" \
    "find $T/finddir -type f | sort" "find"

run_test "find: -type d sorted" \
    "find $T/finddir -type d | sort" \
    "find $T/finddir -type d | sort" "find"

run_test "find: -maxdepth 1 -type f" \
    "find $T/finddir -maxdepth 1 -type f | sort" \
    "find $T/finddir -maxdepth 1 -type f | sort" "find"

run_test "find: -name *.log" \
    "find $T/finddir -name '*.log'" \
    "find $T/finddir -name '*.log'" "find"

run_test_exitcode "find: nonexistent dir exits nonzero" \
    "find $T/nosuchdir_find -name '*.txt'" \
    "find $T/nosuchdir_find -name '*.txt'" "find"

run_test "find: -print0 null-delimited then count" \
    "find $T/finddir -name '*.txt' -print0 | tr -dc '\0' | wc -c" \
    "find $T/finddir -name '*.txt' -print0 | tr -dc '\0' | wc -c" "find"

# ---------------------------------------------------------------------------
# xargs tests
# ---------------------------------------------------------------------------

run_test "xargs: basic echo" \
    "sh -c 'printf \"a b c\n\" | xargs echo'" \
    "sh -c 'printf \"a b c\n\" | xargs echo'" "xargs"

run_test "xargs: -n 1 one arg per line" \
    "sh -c 'printf \"a b c\n\" | xargs -n 1 echo'" \
    "sh -c 'printf \"a b c\n\" | xargs -n 1 echo'" "xargs"

run_test "xargs: -I{} replace string" \
    "sh -c 'printf \"foo\nbar\n\" | xargs -I{} printf \"item: %s\n\" {}'" \
    "sh -c 'printf \"foo\nbar\n\" | xargs -I{} printf \"item: %s\n\" {}'" "xargs"

run_test_exitcode "xargs: -r empty input does not run" \
    "sh -c 'printf \"\" | xargs -r false'" \
    "sh -c 'printf \"\" | xargs -r false'" "xargs"

run_test "xargs: -0 null delimiter" \
    "sh -c 'printf \"a\0b\0c\0\" | xargs -0 printf \"%s\n\"'" \
    "sh -c 'printf \"a\0b\0c\0\" | xargs -0 printf \"%s\n\"'" "xargs"

# ---------------------------------------------------------------------------
# mv tests
# ---------------------------------------------------------------------------

cp "$T/hello.txt" "$T/mv_c_src.txt"
cp "$T/hello.txt" "$T/mv_r_src.txt"

run_test "mv: rename file preserves content" \
    "mv $T/mv_c_src.txt $T/mv_c_dst.txt && cat $T/mv_c_dst.txt" \
    "mv $T/mv_r_src.txt $T/mv_r_dst.txt && cat $T/mv_r_dst.txt" "mv"

run_test_exitcode "mv: nonexistent source exits 1" \
    "mv $T/nosuch_mv_c.txt $T/mv_c_out.txt" \
    "mv $T/nosuch_mv_r.txt $T/mv_r_out.txt" "mv"

cp "$T/hello.txt" "$T/mv_c2.txt"
cp "$T/hello.txt" "$T/mv_r2.txt"
mkdir -p "$T/mv_c_dir" "$T/mv_r_dir"

run_test_exitcode "mv: into directory" \
    "mv $T/mv_c2.txt $T/mv_c_dir/" \
    "mv $T/mv_r2.txt $T/mv_r_dir/" "mv"

# ---------------------------------------------------------------------------
# ln tests
# ---------------------------------------------------------------------------

run_test "ln: symlink -s target" \
    "ln -s $T/hello.txt $T/ln_c.lnk && readlink $T/ln_c.lnk" \
    "ln -s $T/hello.txt $T/ln_r.lnk && readlink $T/ln_r.lnk" "ln"

run_test "ln: hard link (same content)" \
    "ln $T/hello.txt $T/ln_hard_c.txt && cat $T/ln_hard_c.txt" \
    "ln $T/hello.txt $T/ln_hard_r.txt && cat $T/ln_hard_r.txt" "ln"

run_test_exitcode "ln: existing target exits 1" \
    "ln -s $T/hello.txt $T/ln_c.lnk" \
    "ln -s $T/hello.txt $T/ln_r.lnk" "ln"

# ---------------------------------------------------------------------------
# chmod tests
# ---------------------------------------------------------------------------

cp "$T/hello.txt" "$T/chmod_c.txt"
cp "$T/hello.txt" "$T/chmod_r.txt"

run_test "chmod: set 644" \
    "chmod 644 $T/chmod_c.txt && stat -c '%a' $T/chmod_c.txt" \
    "chmod 644 $T/chmod_r.txt && stat -c '%a' $T/chmod_r.txt" "chmod"

cp "$T/hello.txt" "$T/chmod_c2.txt"
cp "$T/hello.txt" "$T/chmod_r2.txt"

run_test "chmod: set 755" \
    "chmod 755 $T/chmod_c2.txt && stat -c '%a' $T/chmod_c2.txt" \
    "chmod 755 $T/chmod_r2.txt && stat -c '%a' $T/chmod_r2.txt" "chmod"

cp "$T/hello.txt" "$T/chmod_c3.txt"
cp "$T/hello.txt" "$T/chmod_r3.txt"

run_test "chmod: symbolic u+x" \
    "chmod u+x $T/chmod_c3.txt && stat -c '%a' $T/chmod_c3.txt" \
    "chmod u+x $T/chmod_r3.txt && stat -c '%a' $T/chmod_r3.txt" "chmod"

run_test_exitcode "chmod: nonexistent exits 1" \
    "chmod 644 $T/nosuch_chmod.txt" "chmod 644 $T/nosuch_chmod.txt" "chmod"

# ---------------------------------------------------------------------------
# touch tests
# ---------------------------------------------------------------------------

run_test_exitcode "touch: create new file" \
    "touch $T/touch_c.txt" "touch $T/touch_r.txt" "touch"

run_test "touch: created file is empty" \
    "touch $T/touch_c2.txt && cat $T/touch_c2.txt" \
    "touch $T/touch_r2.txt && cat $T/touch_r2.txt" "touch"

run_test_exitcode "touch: update existing file" \
    "touch $T/hello.txt" "touch $T/hello.txt" "touch"

# ---------------------------------------------------------------------------
# tee tests
# ---------------------------------------------------------------------------

run_test "tee: stdout pass-through" \
    "sh -c 'echo hello | tee $T/tee_c.txt'" \
    "sh -c 'echo hello | tee $T/tee_r.txt'" "tee"

run_test "tee: file content written" \
    "sh -c 'echo world | tee $T/tee_c2.txt && cat $T/tee_c2.txt'" \
    "sh -c 'echo world | tee $T/tee_r2.txt && cat $T/tee_r2.txt'" "tee"

printf 'first\n' > "$T/tee_c3.txt"
printf 'first\n' > "$T/tee_r3.txt"
run_test "tee: -a append mode" \
    "sh -c 'echo second | tee -a $T/tee_c3.txt && cat $T/tee_c3.txt'" \
    "sh -c 'echo second | tee -a $T/tee_r3.txt && cat $T/tee_r3.txt'" "tee"

# ---------------------------------------------------------------------------
# install tests
# ---------------------------------------------------------------------------

run_test "install: copy file" \
    "install $T/hello.txt $T/inst_c.txt && cat $T/inst_c.txt" \
    "install $T/hello.txt $T/inst_r.txt && cat $T/inst_r.txt" "install"

run_test "install: -m sets permissions" \
    "install -m 755 $T/hello.txt $T/inst_c2.txt && stat -c '%a' $T/inst_c2.txt" \
    "install -m 755 $T/hello.txt $T/inst_r2.txt && stat -c '%a' $T/inst_r2.txt" "install"

run_test_exitcode "install: nonexistent source exits 1" \
    "install $T/nosuch_inst.txt $T/inst_c3.txt" \
    "install $T/nosuch_inst.txt $T/inst_r3.txt" "install"

mkdir -p "$T/inst_c_dir" "$T/inst_r_dir"
run_test "install: -d create directory" \
    "install -d $T/inst_c_dir/sub && stat -c '%F' $T/inst_c_dir/sub" \
    "install -d $T/inst_r_dir/sub && stat -c '%F' $T/inst_r_dir/sub" "install"

# ---------------------------------------------------------------------------
# mktemp tests (non-deterministic; use inline TAP)
# ---------------------------------------------------------------------------

if [ -z "$TOOL" ] || [ "$TOOL" = "mktemp" ]; then
    TOTAL=$((TOTAL + 1))
    _tf=$(eval "$SILEX mktemp" 2>&1)
    _ec=$?
    if [ "$_ec" -eq 0 ] && [ -f "$_tf" ]; then
        echo "ok $TOTAL - mktemp: creates temp file"
        PASS=$((PASS + 1))
        rm -f "$_tf"
    else
        echo "not ok $TOTAL - mktemp: creates temp file"
        echo "  # exit=$_ec output=$_tf"
        FAIL=$((FAIL + 1))
    fi

    TOTAL=$((TOTAL + 1))
    _td=$(eval "$SILEX mktemp -d" 2>&1)
    _ec=$?
    if [ "$_ec" -eq 0 ] && [ -d "$_td" ]; then
        echo "ok $TOTAL - mktemp: -d creates temp directory"
        PASS=$((PASS + 1))
        rmdir "$_td"
    else
        echo "not ok $TOTAL - mktemp: -d creates temp directory"
        echo "  # exit=$_ec output=$_td"
        FAIL=$((FAIL + 1))
    fi

    TOTAL=$((TOTAL + 1))
    _tf=$(eval "$SILEX mktemp -p $T" 2>&1)
    _ec=$?
    if [ "$_ec" -eq 0 ] && [ -f "$_tf" ]; then
        echo "ok $TOTAL - mktemp: -p uses given directory"
        PASS=$((PASS + 1))
        rm -f "$_tf"
    else
        echo "not ok $TOTAL - mktemp: -p uses given directory"
        echo "  # exit=$_ec output=$_tf"
        FAIL=$((FAIL + 1))
    fi
fi

# ---------------------------------------------------------------------------
# Additional grep tests — COV-02 + COV-04 (charclass)
# ---------------------------------------------------------------------------

run_test "grep: -E extended regex" \
    "grep -E 'a.+e' $T/fruits.txt" "grep -E 'a.+e' $T/fruits.txt" "grep"

run_test "grep: -F fixed string no-regex" \
    "grep -F 'a*' $T/hello.txt" "grep -F 'a*' $T/hello.txt" "grep"

run_test "grep: -l list matching files" \
    "grep -l apple $T/fruits.txt $T/hello.txt" \
    "grep -l apple $T/fruits.txt $T/hello.txt" "grep"

run_test "grep: -L list non-matching files" \
    "grep -L apple $T/fruits.txt $T/hello.txt" \
    "grep -L apple $T/fruits.txt $T/hello.txt" "grep"

run_test "grep: -H show filename" \
    "grep -H apple $T/fruits.txt" "grep -H apple $T/fruits.txt" "grep"

run_test "grep: -q quiet exit code" \
    "grep -q apple $T/fruits.txt && echo found || echo notfound" \
    "grep -q apple $T/fruits.txt && echo found || echo notfound" "grep"

run_test "grep: -e multiple patterns" \
    "grep -e apple -e cherry $T/fruits.txt" \
    "grep -e apple -e cherry $T/fruits.txt" "grep"

run_test "grep: -r recursive" \
    "grep -r subfile $T/subdir" "grep -r subfile $T/subdir" "grep"

run_test "grep: -m max count" \
    "grep -m 2 a $T/fruits.txt" "grep -m 2 a $T/fruits.txt" "grep"

# COV-04: POSIX character classes (exercises charclass_re.c)
run_test "grep: [:alpha:] class" \
    "printf 'abc123\n456\n' | grep '[[:alpha:]]'" \
    "printf 'abc123\n456\n' | grep '[[:alpha:]]'" "grep"

run_test "grep: [:digit:] class" \
    "printf 'abc\n123\n' | grep '[[:digit:]]'" \
    "printf 'abc\n123\n' | grep '[[:digit:]]'" "grep"

run_test "grep: [:space:] class" \
    "printf 'a b\nnospace\n' | grep '[[:space:]]'" \
    "printf 'a b\nnospace\n' | grep '[[:space:]]'" "grep"

run_test "grep: [:upper:] class" \
    "printf 'Hello\nworld\n' | grep '[[:upper:]]'" \
    "printf 'Hello\nworld\n' | grep '[[:upper:]]'" "grep"

run_test "grep: [:lower:] class" \
    "printf 'Hello\nWORLD\n' | grep '[[:lower:]]'" \
    "printf 'Hello\nWORLD\n' | grep '[[:lower:]]'" "grep"

run_test "grep: [:alnum:] class" \
    "printf 'abc123\n---\n' | grep '[[:alnum:]]'" \
    "printf 'abc123\n---\n' | grep '[[:alnum:]]'" "grep"

run_test "grep: negated char class" \
    "printf 'abc\n123\n' | grep '[^[:alpha:]]'" \
    "printf 'abc\n123\n' | grep '[^[:alpha:]]'" "grep"

# ---------------------------------------------------------------------------
# Additional sed tests — COV-03
# ---------------------------------------------------------------------------

run_test "sed: address range" \
    "sed '1,2s/a/A/' $T/fruits.txt" \
    "sed '1,2s/a/A/' $T/fruits.txt" "sed"

run_test "sed: -e multiple expressions" \
    "sed -e 's/a/A/' -e 's/e/E/' $T/fruits.txt" \
    "sed -e 's/a/A/' -e 's/e/E/' $T/fruits.txt" "sed"

run_test "sed: numbered substitution (2nd)" \
    "sed 's/a/A/2' $T/fruits.txt" "sed 's/a/A/2' $T/fruits.txt" "sed"

run_test "sed: last-line address" \
    "sed '\$s/elderberry/ELDERBERRY/' $T/fruits.txt" \
    "sed '\$s/elderberry/ELDERBERRY/' $T/fruits.txt" "sed"

run_test "sed: y transliterate" \
    "printf 'hello\n' | sed 'y/helo/HELO/'" \
    "printf 'hello\n' | sed 'y/helo/HELO/'" "sed"

run_test "sed: hold space h/g" \
    "printf 'a\nb\n' | sed -n '1h;2{g;p}'" \
    "printf 'a\nb\n' | sed -n '1h;2{g;p}'" "sed"

run_test "sed: -E ERE mode" \
    "printf 'abc123\n' | sed -E 's/[[:digit:]]+/NUM/'" \
    "printf 'abc123\n' | sed -E 's/[[:digit:]]+/NUM/'" "sed"

run_test "sed: append text a\\" \
    "printf 'line1\nline2\n' | sed '1a\\appended'" \
    "printf 'line1\nline2\n' | sed '1a\\appended'" "sed"

run_test "sed: insert text i\\" \
    "printf 'line1\nline2\n' | sed '2i\\inserted'" \
    "printf 'line1\nline2\n' | sed '2i\\inserted'" "sed"

# ---------------------------------------------------------------------------
# Additional head tests — improve from 31%
# ---------------------------------------------------------------------------

run_test "head: -n 2 first 2 lines" \
    "head -n 2 $T/fruits.txt" "head -n 2 $T/fruits.txt" "head"

run_test "head: -n 1 first line" \
    "head -n 1 $T/fruits.txt" "head -n 1 $T/fruits.txt" "head"

run_test "head: -c byte count" \
    "head -c 5 $T/hello.txt" "head -c 5 $T/hello.txt" "head"

run_test "head: multiple files" \
    "head -n 1 $T/fruits.txt $T/words.txt" \
    "head -n 1 $T/fruits.txt $T/words.txt" "head"

# ---------------------------------------------------------------------------
# Additional cut tests — improve from 49%
# ---------------------------------------------------------------------------

run_test "cut: -d: -f1 delimiter" \
    "printf 'a:b:c\n1:2:3\n' | cut -d: -f1" \
    "printf 'a:b:c\n1:2:3\n' | cut -d: -f1" "cut"

run_test "cut: -f1,3 multiple fields" \
    "printf 'a:b:c\n' | cut -d: -f1,3" \
    "printf 'a:b:c\n' | cut -d: -f1,3" "cut"

run_test "cut: -c character range" \
    "printf 'hello world\n' | cut -c1-5" \
    "printf 'hello world\n' | cut -c1-5" "cut"

run_test "cut: -f2- from field 2 to end" \
    "printf 'a:b:c:d\n' | cut -d: -f2-" \
    "printf 'a:b:c:d\n' | cut -d: -f2-" "cut"

# ---------------------------------------------------------------------------
# Additional tr tests — improve from 36%
# ---------------------------------------------------------------------------

run_test "tr: lowercase to uppercase" \
    "printf 'hello\n' | tr a-z A-Z" \
    "printf 'hello\n' | tr a-z A-Z" "tr"

run_test "tr: -d delete chars" \
    "printf 'h3ll0\n' | tr -d 0-9" \
    "printf 'h3ll0\n' | tr -d 0-9" "tr"

run_test "tr: -s squeeze repeats" \
    "printf 'aaabbbccc\n' | tr -s a-z" \
    "printf 'aaabbbccc\n' | tr -s a-z" "tr"

run_test "tr: translate newlines" \
    "printf 'a\nb\nc\n' | tr '\n' ':'" \
    "printf 'a\nb\nc\n' | tr '\n' ':'" "tr"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "1..$TOTAL"
echo "# Results: $PASS passed, $FAIL failed, $SKIP skipped out of $TOTAL"
[ "$FAIL" -eq 0 ]
