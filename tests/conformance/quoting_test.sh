#!/bin/sh
# quoting_test.sh — Q-01 through Q-08: quoting/expansion/subshell tests
# Runs each case through silex AND dash, diffs output.
# Any diff where dash is correct = silex bug.

SILEX="${SILEX:-$(dirname "$0")/../../build/bin/silex}"
DASH="${DASH:-/usr/bin/dash}"
PASS=0; FAIL=0; SKIP=0

run_case() {
    local id="$1" desc="$2" script="$3"
    local mb_out dash_out
    mb_out=$(echo "$script" | "$SILEX" sh 2>&1)
    dash_out=$(echo "$script" | "$DASH" 2>&1)
    if [ "$mb_out" = "$dash_out" ]; then
        printf 'PASS [%s] %s\n' "$id" "$desc"
        PASS=$((PASS+1))
    else
        printf 'FAIL [%s] %s\n' "$id" "$desc"
        printf '  dash:    %s\n' "$dash_out"
        printf '  silex:%s\n' "$mb_out"
        FAIL=$((FAIL+1))
    fi
}

echo "=== Q-01: Nested quoting ==="
run_case Q-01-1 "double around single" "echo \"hello 'world'\""
run_case Q-01-2 "single around double" "echo 'hello \"world\"'"
run_case Q-01-3 "escaped double inside double" 'echo "hello \"world\""'
run_case Q-01-4 "cmd sub inside double" 'echo "$(echo "nested")"'
run_case Q-01-5 "doubly nested cmd sub" 'echo "$(echo "$(echo "deep")")"'
run_case Q-01-6 "nested cmd sub in middle" 'echo "a$(echo "b$(echo "c")")d"'
run_case Q-01-7 "variable with quotes" 'x='"'"'"quoted"'"'"'; echo "$x"'
run_case Q-01-8 "variable with apostrophe" "x=\"it's\"; echo \"\$x\""

echo "=== Q-02: Word splitting with IFS ==="
run_case Q-02-1 "colon IFS empty field" 'IFS=:; x="a:b::c"; set -- $x; echo $#'
run_case Q-02-2 "space IFS collapses" 'IFS=" "; x="  a  b  "; set -- $x; echo $#'
run_case Q-02-3 "empty IFS no split" 'IFS=""; x="abc"; set -- $x; echo $#'
run_case Q-02-4 "unset IFS default" 'unset IFS; x="a b	c"; set -- $x; echo $#'

echo "=== Q-03: Parameter expansion ==="
run_case Q-03-1  "unset var dash"    'unset x; echo "${x-default}"'
run_case Q-03-2  "unset var colon-dash" 'unset x; echo "${x:-default}"'
run_case Q-03-3  "empty var dash"    'x=""; echo "${x-default}"'
run_case Q-03-4  "empty var colon-dash" 'x=""; echo "${x:-default}"'
run_case Q-03-5  "set var plus"      'x="hi"; echo "${x+set}"'
run_case Q-03-6  "empty var colon-plus" 'x=""; echo "${x:+set}"'
run_case Q-03-7  "strip suffix"      'x="hello world"; echo "${x%world}"'
run_case Q-03-8  "strip suffix greedy" 'x="hello world"; echo "${x%%o*}"'
run_case Q-03-9  "strip prefix"      'x="hello world"; echo "${x#hello}"'
run_case Q-03-10 "strip prefix greedy" 'x="hello world"; echo "${x##*o}"'
run_case Q-03-11 "strip path base"   'x="/path/to/file.tar.gz"; echo "${x##*/}"'
run_case Q-03-12 "strip path dir"    'x="/path/to/file.tar.gz"; echo "${x%/*}"'
run_case Q-03-13 "strip extension"   'x="/path/to/file.tar.gz"; echo "${x%.gz}"'
run_case Q-03-14 "strip all after dot" 'x="/path/to/file.tar.gz"; echo "${x%%.*}"'
run_case Q-03-15 "string length"     'x="/path/to/file.tar.gz"; echo "${#x}"'

echo "=== Q-04: Command substitution ==="
run_case Q-04-1  "basic dollar-paren" 'echo $(echo hello)'
run_case Q-04-2  "nested dollar-paren" 'echo $(echo $(echo nested))'
run_case Q-04-3  "deep nested in quotes" 'echo "$(echo "$(echo "deep")")"'
run_case Q-04-4  "trailing newline stripped" 'x=$(printf "hello\n\n\n"); echo "${#x}"'
run_case Q-04-5  "exit code propagation false" 'x=$(false); echo $?'
run_case Q-04-6  "exit code propagation true" 'x=$(true); echo $?'
run_case Q-04-7  "in arithmetic" 'echo $(( $(echo 5) + $(echo 3) ))'
run_case Q-04-8  "backtick form" 'echo `echo hello`'

echo "=== Q-05: Here-doc ==="
run_case Q-05-1 "tab stripping with <<-" 'cat <<-EOF
	indented
EOF'
run_case Q-05-2 "quoted delimiter no expand" 'x=HELLO
cat <<'"'"'EOF'"'"'
$x
EOF'
run_case Q-05-3 "unquoted delimiter expands" 'x=HELLO
cat <<EOF
$x
EOF'
run_case Q-05-4 "heredoc piped" 'cat <<EOF | grep hello
hello world
goodbye
EOF'
run_case Q-05-5 "heredoc in cmd sub" 'x=$(cat <<EOF
content
EOF
)
echo "$x"'
run_case Q-05-6 "delimiter in content" 'cat <<EOF
This line contains EOF in the middle
EOF'
run_case Q-05-7 "empty body" 'cat <<EOF
EOF'

echo "=== Q-06: Case statement ==="
run_case Q-06-1 "pattern precedence" 'case "hello" in h*) echo "glob";; hello) echo "exact";; esac'
run_case Q-06-2 "multiple patterns" 'case "b" in a|b|c) echo "match";; esac'
run_case Q-06-3 "POSIX negation" 'case "5" in [!a-z]) echo "non-alpha";; esac'
run_case Q-06-4 "quoted pattern literal" 'case "h*" in "h*") echo "literal";; h*) echo "glob";; esac'

echo "=== Q-07: Subshell isolation ==="
run_case Q-07-1 "variable isolation" 'x=1; (x=2; echo $x); echo $x'
run_case Q-07-2 "exit in subshell" '(exit 1); echo "here: $?"'
run_case Q-07-3 "trap isolation" 'trap "echo parent" EXIT
(trap "echo child" EXIT)
trap - EXIT'

echo "=== Q-08: eval ==="
run_case Q-08-1 "basic eval" 'eval "echo hello"'
run_case Q-08-2 "eval with variable" 'eval '"'"'echo $HOME'"'"' | wc -c | tr -d " "'
run_case Q-08-3 "eval preserves quoting" 'cmd='"'"'echo "a b"'"'"'; eval "$cmd"'
run_case Q-08-4 "eval sets variable" 'eval "x=5"; echo $x'
run_case Q-08-5 "eval with cmd sub" 'eval '"'"'echo "$(echo nested)"'"'"''

printf '\nQuoting conformance: %d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
