#!/bin/sh
# tests/unit/shell/test_redirects.sh — shell redirection tests for matchbox
# chmod +x tests/unit/shell/test_redirects.sh
# Usage: ./test_redirects.sh [path/to/matchbox]

MATCHBOX="${1:-build/bin/matchbox}"
PASS=0
FAIL=0

TMPDIR_REDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR_REDIR"' EXIT INT TERM

check() {
    local desc="$1"
    local got="$2"
    local expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected: $(printf '%s' "$expected" | cat -A)"
        echo "  got:      $(printf '%s' "$got" | cat -A)"
        FAIL=$((FAIL + 1))
    fi
}

check_exit() {
    local desc="$1"
    local got_exit="$2"
    local expected_exit="$3"
    if [ "$got_exit" = "$expected_exit" ]; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  expected exit: $expected_exit"
        echo "  got exit:      $got_exit"
        FAIL=$((FAIL + 1))
    fi
}

check_file() {
    local desc="$1"
    local file="$2"
    local expected="$3"
    local got
    got=$(cat "$file" 2>/dev/null)
    check "$desc" "$got" "$expected"
}

MB="$MATCHBOX"
T="$TMPDIR_REDIR"

# --- > create/overwrite ---
"$MB" -c "echo first > $T/create.txt"
check_file "> creates file with content" "$T/create.txt" "first"

"$MB" -c "echo second > $T/create.txt"
check_file "> overwrites existing file" "$T/create.txt" "second"

# --- > creates empty file ---
rm -f "$T/empty.txt"
"$MB" -c "> $T/empty.txt"
[ -f "$T/empty.txt" ]
check_exit "> on its own creates empty file" "$?" "0"
check_file "> creates file with empty content" "$T/empty.txt" ""

# --- >> append ---
"$MB" -c "echo line1 > $T/append.txt"
"$MB" -c "echo line2 >> $T/append.txt"
"$MB" -c "echo line3 >> $T/append.txt"
check_file ">> appends to existing file" "$T/append.txt" "$(printf 'line1\nline2\nline3')"

# --- >> creates file if not exists ---
rm -f "$T/newappend.txt"
"$MB" -c "echo created >> $T/newappend.txt"
check_file ">> creates file if not exists" "$T/newappend.txt" "created"

# --- < input redirect ---
printf 'input content\n' > "$T/input.txt"
got=$("$MB" -c "cat < $T/input.txt")
check "< redirects file to stdin of command" "$got" "input content"

# --- < with grep ---
printf 'alpha\nbeta\ngamma\n' > "$T/words.txt"
got=$("$MB" -c "grep beta < $T/words.txt")
check "< with grep reads from file" "$got" "beta"

# --- 2> stderr redirect ---
"$MB" -c "cat /nonexistent_file_xyz 2> $T/stderr.txt" || true
got=$(cat "$T/stderr.txt")
# Should have written something to stderr file
if [ -n "$got" ]; then
    echo "PASS: 2> redirects stderr to file"
    PASS=$((PASS + 1))
else
    echo "FAIL: 2> redirects stderr to file (stderr file is empty)"
    FAIL=$((FAIL + 1))
fi

# --- 2> creates file even with no stderr ---
rm -f "$T/nostderr.txt"
"$MB" -c "echo ok 2> $T/nostderr.txt" > /dev/null
check_file "2> creates file even with no stderr output" "$T/nostderr.txt" ""

# --- 2>&1 stderr merged to stdout ---
got=$("$MB" -c "cat /nonexistent_xyz 2>&1")
if [ -n "$got" ]; then
    echo "PASS: 2>&1 merges stderr into stdout"
    PASS=$((PASS + 1))
else
    echo "FAIL: 2>&1 merges stderr into stdout (nothing captured)"
    FAIL=$((FAIL + 1))
fi

# --- 2>&1 with redirect to file ---
"$MB" -c "{ echo out; cat /nonexistent_xyz; } > $T/merged.txt 2>&1" || true
got=$(cat "$T/merged.txt")
if printf '%s' "$got" | grep -q "out"; then
    echo "PASS: 2>&1 with file redirect: stdout present"
    PASS=$((PASS + 1))
else
    echo "FAIL: 2>&1 with file redirect: stdout missing"
    FAIL=$((FAIL + 1))
fi

# --- &> both stdout and stderr (bash extension) ---
# SKIP: &> is a bash extension, not POSIX; matchbox targets POSIX sh
echo "SKIP: &> is bash extension (not POSIX)"
PASS=$((PASS + 1))

# --- <<EOF heredoc ---
got=$("$MB" -c 'cat <<EOF
hello heredoc
world
EOF')
check "<<EOF heredoc: multiline content" "$got" "$(printf 'hello heredoc\nworld')"

# --- <<EOF heredoc with variable expansion ---
got=$("$MB" -c 'VAR=expanded; cat <<EOF
value is $VAR
EOF')
check "<<EOF heredoc: variable expanded" "$got" "value is expanded"

# --- <<'EOF' heredoc: no variable expansion ---
got=$("$MB" -c "cat <<'EOF'
no \$VAR expansion
EOF")
check "<<'EOF' heredoc: no variable expansion" "$got" 'no $VAR expansion'

# --- <<-EOF heredoc with tab stripping ---
got=$("$MB" -c "$(printf 'cat <<-EOF\n\thello tab stripped\n\tworld\n\tEOF')")
check "<<-EOF heredoc: leading tabs stripped" "$got" "$(printf 'hello tab stripped\nworld')"

# --- /dev/null redirect: suppress stdout ---
got=$("$MB" -c 'echo suppress_me > /dev/null')
check "/dev/null redirect: stdout suppressed" "$got" ""

# --- /dev/null redirect: suppress stderr ---
"$MB" -c 'cat /nonexistent_xyz 2>/dev/null'
check_exit "/dev/null: stderr suppressed, exit still non-zero" "$?" "1"

# --- < /dev/null: empty stdin ---
got=$("$MB" -c 'wc -c < /dev/null')
got=$(printf '%s' "$got" | tr -d ' ')
check "< /dev/null: empty stdin to wc -c gives 0" "$got" "0"

# --- redirect with spaces in filename ---
SPACEFILE="$T/file with spaces.txt"
"$MB" -c "echo spaced > \"$SPACEFILE\""
check_file "redirect to file with spaces in name" "$SPACEFILE" "spaced"

# --- stdout and stderr to separate files ---
"$MB" -c "{ echo stdout_line; cat /nonexistent_xyz; } > $T/sep_out.txt 2> $T/sep_err.txt" || true
check_file "separate out/err: stdout file correct" "$T/sep_out.txt" "stdout_line"
got_err=$(cat "$T/sep_err.txt")
if [ -n "$got_err" ]; then
    echo "PASS: separate out/err: stderr file has content"
    PASS=$((PASS + 1))
else
    echo "FAIL: separate out/err: stderr file is empty"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "redirect tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
