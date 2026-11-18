#!/bin/sh
# tests/unit/shell/test_redirects.sh — shell redirection tests for silex
# chmod +x tests/unit/shell/test_redirects.sh
# Usage: ./test_redirects.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
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

MB="$SILEX"
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
# SKIP: &> is a bash extension, not POSIX; silex targets POSIX sh
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

# -----------------------------------------------------------------------
# fd management must not intrude on the user's fd range (0-9).
# -----------------------------------------------------------------------
# `exec 3>file` must write to the FILE. open() returns fd 3 (the lowest free
# fd once 0-2 are open), i.e. the target itself; the code used to dup2(3,3)
# then close(3), silently discarding the redirection so the later write went
# to stdout instead of the file.
check "exec 3>file writes to the file, not stdout" \
    "$("$SILEX" -c "exec 3>$TMPDIR_REDIR/e3.txt; echo VVV >&3; exec 3>&-; cat $TMPDIR_REDIR/e3.txt")" \
    "VVV"
check "exec 3>file leaves nothing on stdout" \
    "$("$SILEX" -c "exec 3>$TMPDIR_REDIR/e3b.txt; echo VVV >&3; exec 3>&-" 2>/dev/null)" \
    ""
# A saved fd must be parked high (>=10), or the block's save of fd 2 lands on a
# user fd (3, 4, ...) and makes a `>&N` to a genuinely-closed N spuriously
# succeed. dash/bash close fd 4 for the block, so `true >&4` fails (rc!=0).
check_exit "\`{ true >&4; } 2>/dev/null\` fails: fd 4 really is closed" \
    "$("$SILEX" -c '{ true >&4; } 2>/dev/null; echo $?')" \
    "1"
check "exec 4>/dev/null inside a { } 4>&- block does not leak out" \
    "$("$SILEX" -c '{ exec 4>/dev/null; } 4>&-; { true >&4; } 2>/dev/null && echo OPEN || echo closed')" \
    "closed"
# A block that redirects a live user fd must save and restore it correctly.
check "block redirect of a live fd 3 is saved and restored" \
    "$("$SILEX" -c "exec 3>$TMPDIR_REDIR/a.txt; { echo B >&3; } 3>$TMPDIR_REDIR/b.txt; echo C >&3; exec 3>&-; echo \"a=[\$(cat $TMPDIR_REDIR/a.txt)] b=[\$(cat $TMPDIR_REDIR/b.txt)]\"")" \
    "a=[C] b=[B]"

# --- IO_NUMBER requires the operator to be adjacent (POSIX 2.10.2 rule 2) ---
# `N>` is a file descriptor; `N >` is an ordinary argument followed by a
# redirect of stdout. Treating them alike silently ATE the last argument of
# every command whose final operand was numeric: `seq 5 6 > f` ran `seq 5` and
# pointed fd 6 at the file, and `f 512 > out` called f with no arguments.
check "a digit word separated by a blank is an argument, not an fd" \
    "$("$SILEX" -c "seq 5 6 > $TMPDIR_REDIR/io.txt; cat $TMPDIR_REDIR/io.txt" | tr '\n' ' ')" \
    "5 6 "
check "an adjacent digit IS an fd" \
    "$("$SILEX" -c "echo hi 2>$TMPDIR_REDIR/io2.txt; echo -n; test -s $TMPDIR_REDIR/io2.txt && echo nonempty || echo empty")" \
    "hi
empty"
check "words after a mid-command redirect are kept" \
    "$("$SILEX" -c "echo a 5 >$TMPDIR_REDIR/io3.txt b; cat $TMPDIR_REDIR/io3.txt")" \
    "a 5 b"

# --- a redirect may FOLLOW a compound command, never prefix one ---
# `>f for i in ...` has no production in the grammar. Parsing it as a
# redirect-only command left the loop running UNREDIRECTED -- output went to
# the terminal while the script believed it was captured.
"$SILEX" -c '>'"$TMPDIR_REDIR"'/pre.txt for i in 1 2; do echo $i; done' >/dev/null 2>&1
check_exit "redirect prefixing a for loop is a syntax error" "$?" "2"
check "a suffix redirect on the same loop still works" \
    "$("$SILEX" -c 'for i in 1 2; do echo $i; done > '"$TMPDIR_REDIR"'/suf.txt; cat '"$TMPDIR_REDIR"'/suf.txt' | tr '\n' ' ')" \
    "1 2 "

# --- stdin redirects into in-process applets -------------------------------
# An applet inside silex sh is a function call reading the SHELL's stdio
# `stdin`. Its read-ahead buffer and EOF flag used to survive into the next
# command: the second `tr <f` printed nothing and still exited 0.
printf 'l1\nl2\nl3\n' > "$T/in3"
printf 'm1\nm2\n' > "$T/in2"
got=$("$MB" -c "tr a-z A-Z <'$T/in3'; echo rc=\$?; tr a-z A-Z <'$T/in3'; echo rc=\$?" | tr '\n' ' ')
check "<file into an applet works on every run, not just the first" "$got" "L1 L2 L3 rc=0 L1 L2 L3 rc=0 "
got=$("$MB" -c "head -n1 <'$T/in3'; head -n1 <'$T/in2'; cat <'$T/in2'" | tr '\n' ' ')
check "a later <file sees its own file, not the previous one's leftovers" "$got" "l1 m1 m1 m2 "
got=$("$MB" -c "i=0; while [ \$i -lt 50 ]; do wc -l <'$T/in3'; i=\$((i+1)); done" | sort -u | tr -d ' ')
check "50 x wc -l <file in one shell: always 3" "$got" "3"

# --- one file split across commands ------------------------------------------
# POSIX: a utility that stops reading a seekable input early leaves the offset
# just past what it processed. head's stdio read-ahead used to swallow the
# whole file, so the command after it got nothing.
got=$("$MB" -c "{ head -n1; cat; } <'$T/in3'" | tr '\n' ' ')
check "{ head -n1; cat; } <f: head takes line 1, cat the rest" "$got" "l1 l2 l3 "
got=$("$MB" -c "{ head -n1; head -n1; head -n1; } <'$T/in3'" | tr '\n' ' ')
check "three head -n1 share one file" "$got" "l1 l2 l3 "
got=$("$MB" -c "{ sed 1q; tr a-z A-Z; } <'$T/in3'" | tr '\n' ' ')
check "{ sed 1q; tr; } <f" "$got" "l1 L2 L3 "
got=$("$MB" -c "{ grep -m1 l; cat; } <'$T/in3'" | tr '\n' ' ')
check "{ grep -m1; cat; } <f: grep stops after its match" "$got" "l1 l2 l3 "
got=$("$MB" -c "{ head -n1; tail -n1; } <'$T/in3'" | tr '\n' ' ')
check "{ head -n1; tail -n1; } <f" "$got" "l1 l3 "
got=$("$MB" -c "{ head -n1 >/dev/null; tail -n5; } <'$T/in3'" | tr '\n' ' ')
check "tail -n N larger than the rest starts where the input starts" "$got" "l2 l3 "
got=$("$MB" -c "{ head -n2 >/dev/null; tail -c 100; } <'$T/in3'" | tr '\n' ' ')
check "tail -c N larger than the rest starts where the input starts" "$got" "l3 "
got=$("$MB" -c "{ head -n1 >/dev/null; sort -r; } <'$T/in3'" | tr '\n' ' ')
check "{ head -n1; sort -r; } <f" "$got" "l3 l2 "
got=$("$MB" -c "{ head -n1; wc -l; } <'$T/in3'" | tr -d ' ' | tr '\n' ' ')
check "{ head -n1; wc -l; } <f" "$got" "l1 2 "
got=$("$MB" -c "{ head -n1; sh -c 'head -n1'; cat; } <'$T/in3'" | tr '\n' ' ')
check "a forked applet (sh -c) leaves the shared offset right too" "$got" "l1 l2 l3 "
got=$("$MB" -c "exec <'$T/in3'; head -n1; read -r x; echo \"read=\$x\"; cat" | tr '\n' ' ')
check "exec <f: head, then the read builtin, then cat" "$got" "l1 read=l2 l3 "
# A file bigger than sort's mmap threshold (64 KB): sort must start at the
# current offset, not map the file from byte 0.
i=0; : > "$T/big"
while [ $i -lt 9000 ]; do echo "line-$i-xxxxxxxx"; i=$((i+1)); done >> "$T/big"
got=$("$MB" -c "{ head -n1 >/dev/null; sort | wc -l; } <'$T/big'" | tr -d ' ')
check "{ head -n1; sort; } <bigfile: sort sees only the rest" "$got" "8999"
got=$("$MB" -c "{ head -n1 >/dev/null; sort >/dev/null; cat | wc -c; } <'$T/big'" | tr -d ' ')
check "sort reading via mmap leaves the offset at EOF" "$got" "0"

echo ""
echo "redirect tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
