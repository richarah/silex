#!/bin/sh
# test_vcs.sh — tests for --vcs, -S, --changed-within, vcsignore

MATCHBOX="${MATCHBOX:-$(dirname "$0")/../../build/bin/matchbox}"
PASS=0; FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        printf 'PASS: %s\n' "$desc"; PASS=$((PASS+1))
    else
        printf 'FAIL: %s\n  expected: %s\n  got:      %s\n' \
               "$desc" "$expected" "$actual"; FAIL=$((FAIL+1))
    fi
}

check_nonempty() {
    local desc="$1" actual="$2"
    if [ -n "$actual" ]; then
        printf 'PASS: %s\n' "$desc"; PASS=$((PASS+1))
    else
        printf 'FAIL: %s (got empty output)\n' "$desc"; FAIL=$((FAIL+1))
    fi
}

check_empty() {
    local desc="$1" actual="$2"
    if [ -z "$actual" ]; then
        printf 'PASS: %s\n' "$desc"; PASS=$((PASS+1))
    else
        printf 'FAIL: %s (expected empty, got: %s)\n' "$desc" "$actual"; FAIL=$((FAIL+1))
    fi
}

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# Setup: VCS dirs and regular files
mkdir -p "$T/.git" "$T/.svn" "$T/src" "$T/node_modules" "$T/__pycache__"
echo "vcs_secret" > "$T/.git/config"
echo "vcs_secret" > "$T/.svn/entries"
echo "vcs_secret" > "$T/node_modules/pkg.js"
echo "vcs_secret" > "$T/__pycache__/mod.pyc"
echo "hello world" > "$T/src/main.c"
echo "hello world" > "$T/.hidden_file"
printf '\000binary\000' > "$T/src/data.bin"
printf '*.o\nbuild/\n' > "$T/.gitignore"
mkdir -p "$T/build"
echo "generated" > "$T/build/output"

echo "--- grep -S smart case ---"

# Lowercase pattern should match case-insensitively
check "grep -S: lowercase matches mixed case" \
    "Hello World" \
    "$(printf 'Hello World\n' | "$MATCHBOX" grep -S 'hello world')"

# Uppercase pattern should NOT fold
check_empty "grep -S: uppercase stays case-sensitive" \
    "$(printf 'Hello World\n' | "$MATCHBOX" grep -S 'HELLO')"

# Explicit -i always wins over -S with uppercase
check_nonempty "grep -S -i: explicit -i with uppercase matches" \
    "$(printf 'Hello World\n' | "$MATCHBOX" grep -S -i 'HELLO')"

# Mixed pattern with uppercase stays case-sensitive
check_empty "grep -S: mixed-case pattern stays sensitive" \
    "$(printf 'hello world\n' | "$MATCHBOX" grep -S 'Hello')"

echo "--- grep --vcs recursive ---"

# --vcs should skip .git directory
check_empty "grep --vcs: skips .git/" \
    "$("$MATCHBOX" grep --vcs 'vcs_secret' "$T" 2>/dev/null | grep '\.git')"

# --vcs should skip .svn directory
check_empty "grep --vcs: skips .svn/" \
    "$("$MATCHBOX" grep --vcs 'vcs_secret' "$T" 2>/dev/null | grep '\.svn')"

# --vcs should skip node_modules
check_empty "grep --vcs: skips node_modules/" \
    "$("$MATCHBOX" grep --vcs 'vcs_secret' "$T" 2>/dev/null | grep 'node_modules')"

# --vcs should skip hidden files
check_empty "grep --vcs: skips hidden files" \
    "$("$MATCHBOX" grep --vcs 'hello world' "$T" 2>/dev/null | grep '\.hidden')"

# --vcs should still find regular source files
check_nonempty "grep --vcs: finds regular files" \
    "$("$MATCHBOX" grep --vcs 'hello world' "$T" 2>/dev/null)"

# --vcs should skip binary files
check_empty "grep --vcs: skips binary files" \
    "$("$MATCHBOX" grep --vcs 'binary' "$T" 2>/dev/null)"

echo "--- grep --vcs with .gitignore ---"

# .gitignore says build/ is ignored; --vcs should skip it
check_empty "grep --vcs: respects .gitignore build/" \
    "$("$MATCHBOX" grep --vcs 'generated' "$T" 2>/dev/null)"

echo "--- find -S smart case ---"

mkdir -p "$T/ftest"
touch "$T/ftest/Makefile" "$T/ftest/readme.md" "$T/ftest/NOTES.TXT"

# Lowercase pattern should match case-insensitively
check_nonempty "find -S: lowercase pattern matches mixed-case filename" \
    "$("$MATCHBOX" find "$T/ftest" -S -name 'makefile')"

check_empty "find -S: should NOT match with -S and uppercase 'MAKEFILE'" \
    "$("$MATCHBOX" find "$T/ftest" -S -name 'MAKEFILE' 2>/dev/null | grep -i makefile)"

# Exact case always works
check_nonempty "find -S: exact-case pattern still works" \
    "$("$MATCHBOX" find "$T/ftest" -S -name 'Makefile')"

echo "--- find --vcs ---"

mkdir -p "$T/fvcs/.git" "$T/fvcs/src"
touch "$T/fvcs/.git/config" "$T/fvcs/src/main.c" "$T/fvcs/.hidden"

# --vcs skips .git
check_empty "find --vcs: skips .git/" \
    "$("$MATCHBOX" find "$T/fvcs" --vcs -name 'config' 2>/dev/null)"

# --vcs skips hidden
check_empty "find --vcs: skips hidden files" \
    "$("$MATCHBOX" find "$T/fvcs" --vcs -name '.hidden' 2>/dev/null)"

# --vcs finds regular files
check_nonempty "find --vcs: finds regular source files" \
    "$("$MATCHBOX" find "$T/fvcs" --vcs -name 'main.c' 2>/dev/null)"

echo "--- find --changed-within ---"

touch "$T/new_file"

check_nonempty "find --changed-within 10s: finds just-created file" \
    "$("$MATCHBOX" find "$T" --changed-within 10s -name 'new_file' 2>/dev/null)"

check_empty "find --changed-within 0s: invalid spec rejected" \
    "$("$MATCHBOX" find "$T" --changed-within 0s -name 'new_file' 2>/dev/null)"

check_nonempty "find --changed-within 1m: finds recent file" \
    "$("$MATCHBOX" find "$T" --changed-within 1m -name 'new_file' 2>/dev/null)"

check_nonempty "find --changed-within 1h: finds recent file" \
    "$("$MATCHBOX" find "$T" --changed-within 1h -name 'new_file' 2>/dev/null)"

# Very old time window: a file created 10s ago should NOT match
# We can't easily create an old file, so just test that the syntax parses
"$MATCHBOX" find "$T" --changed-within 1w -name 'new_file' > /dev/null 2>&1
check "find --changed-within 1w: syntax valid (exit 0)" "0" "$?"

echo "--- MATCHBOX_SMART env var ---"

# MATCHBOX_SMART=1 should enable smart case for grep
check_nonempty "MATCHBOX_SMART=1: grep lowercase matches mixed-case" \
    "$(printf 'Hello World\n' | MATCHBOX_SMART=1 "$MATCHBOX" grep 'hello world')"

check_empty "MATCHBOX_SMART=1: grep uppercase stays sensitive" \
    "$(printf 'Hello World\n' | MATCHBOX_SMART=1 "$MATCHBOX" grep 'HELLO')"

# MATCHBOX_SMART=1 should enable vcs for find
check_nonempty "MATCHBOX_SMART=1: find skips VCS and finds src files" \
    "$(MATCHBOX_SMART=1 "$MATCHBOX" find "$T/fvcs" -name 'main.c' 2>/dev/null)"

check_empty "MATCHBOX_SMART=1: find skips .git/" \
    "$(MATCHBOX_SMART=1 "$MATCHBOX" find "$T/fvcs" -name 'config' 2>/dev/null)"

printf '\nvcs tests: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
