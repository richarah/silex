#!/bin/bash
# test_grep_engine.sh — the silex regex engine behind grep: GNU syntax and
# extensions, -w/-o semantics, the block scanner and its fast paths.
# Expected outputs are what GNU grep 3.11 prints (C locale).
# Usage: test_grep_engine.sh [path/to/silex]

SILEX="${1:-build/bin/silex}"
case "$SILEX" in /*) ;; *) SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;; esac
PASS=0; FAIL=0
export LC_ALL=C

# Two checks depend on the system: a GNU grep as the reference (BusyBox grep has no
# -b with the same meaning), and a libc whose regcomp has ERE back-references
# (musl has none, so grep -E '(ab)\1' cannot match there, before and after the
# regex engine rewrite).
GNU_GREP=0; grep --version 2>&1 | grep -q 'GNU grep' && GNU_GREP=1
IS_MUSL=0; ls /lib/ld-musl-* >/dev/null 2>&1 && IS_MUSL=1
skip() { echo "  SKIP: $1"; }

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS+1))
    else
        echo "  FAIL: $desc (expected='$expected' got='$actual')"
        FAIL=$((FAIL+1))
    fi
}

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# G INPUT ARGS...: run silex grep on INPUT (printf format), outputs joined by |
G() { local in="$1"; shift; printf -- "$in" > "$T/in"; "$SILEX" grep "$@" "$T/in" 2>&1 | paste -sd'|' -; }
# R INPUT ARGS...: exit status only
R() { local in="$1"; shift; printf -- "$in" > "$T/in"; "$SILEX" grep "$@" "$T/in" >/dev/null 2>&1; echo $?; }

echo "--- GNU extensions ---"
check '\< word start' 'world|worldly' "$(G 'hello world\nworldly\n' -o '\<w\w*')"
check '\> word end' 'o' "$(G 'hello world\n' -o 'o\>')"
check '\b' '1' "$(G 'foo bar\nfoobar\n' -c '\bbar')"
check '\B' 'foobar' "$(G 'foo bar\nfoobar\n' '\Bbar')"
check '\B on an empty line' '1' "$(G '\n' -c '\B')"
check '\b on an empty line' '0' "$(G '\n' -c '\b')"
check '\s' '1' "$(G 'a b\nab\n' -c '\s')"
check '\S\+' 'a|b|ab' "$(G 'a b\nab\n' -o '\S\+')"
check '\w*' 'word_1|x' "$(G 'word_1 x\n' -o '\w*')"
check '\W' '1' "$(G 'x_1 y\n' -c '\W')"
check '\` and '"\\'"' as line anchors' 'a|c' "$(G 'abc\n' -o -e '\`a' -e "c\\'")"
check '\<x in ERE' 'x1' "$(G 'ax1 x1\n' -oE '\<x[0-9]')"
check '[\w] is a bracket of \ and w' '\|w' "$(G 'a\\b w\n' -o '[\w]')"

echo "--- matchers ---"
check '-G is BRE' 'a|b' "$(G 'a|b\nab\n' -G 'a|b')"
check '--basic-regexp' 'ab' "$(G 'ab\n' --basic-regexp 'a\|x')"
check '-G -E conflict' '2' "$(R 'a\n' -G -E a)"
check '-G -F conflict message' 'silex: grep: conflicting matchers specified' "$(G 'a\n' -G -F a)"

echo "--- -w and -o (GNU semantics) ---"
check '-w selects a line when a shorter match is a word' 'foo bar1' "$(G 'foo bar1\n' -w '[a-z ]*')"
check '-ow tries shorter matches at the same start' 'foo' "$(G 'foo bar1\n' -ow '[a-z ]*')"
check '-w: an empty match between non-word chars' '-a' "$(G '-a\n' -w -- '-*')"
check '-ow after an earlier match cuts the retry short (GNU)' 'a|bb' \
    "$(G 'a.b foobar bb a.b a.b\n' -owE -e '..' -e 'a')"
check '-ow with alternatives of different length' 'foobar|foo' "$(G 'foobar foo\n' -ow 'foo\|foobar')"
check '-ow skips non-words' 'foo' "$(G 'xfoo foo foox\n' -ow foo)"
check '-w literal, two patterns' 'foobar x|foo' "$(G 'foobar x\nfoo\nxfoo foo_\n' -w -e foo -e foobar)"
check '-wi literal' 'foo|FOO bar' "$(G 'foobar x\nfoo\nFOO bar\n' -wi foo)"
check '-x literal' 'foo' "$(G 'foo\nfoox\n' -x foo)"
check '-xi literal' 'foo' "$(G 'foo\nfoox\n' -xi FOO)"
check '-x regex' 'ab|abab' "$(G 'ab\nabab\naba\n' -xE '(ab)+')"
check '-ox prints nothing for an empty line' '' "$(G '\n' -ox '')"
check '-o -b: offset of each match' '1:a|4:a' "$(G 'xa ya\n' -ob a)"
check '-n -b order is LINE:OFFSET' '2:3:xy' "$(G 'ab\nxy\n' -nb y)"
check '-o leftmost-longest across patterns' 'ab' "$(G 'abc\n' -o -e b -e ab)"
check '-o skips empty matches' 'ab|cd' "$(G 'ab cd\n' -o '[a-z]*')"

echo "--- patterns ---"
check 'a newline in a pattern makes two patterns' 'foo|bar' "$(G 'foo\nbar\nbaz\n' 'foo
bar')"
check 'a trailing newline adds the empty pattern' '2' "$(G 'a\nb\n' -c 'a
')"
check '-f /dev/null: no patterns, no match' '1' "$(R 'a\n' -f /dev/null)"
check '-v -f /dev/null selects every line' 'a|b' "$(G 'a\nb\n' -v -f /dev/null)"
check '-c -f /dev/null prints nothing (GNU quits at once)' '' "$(G 'a\n' -c -f /dev/null)"
check '-v with an empty pattern prints nothing, even with -c' '' "$(G 'a\nb\n' -vc '')"
check '-m 0 reads nothing' '1|' "$(R 'a\n' -m 0 a)|$(G 'a\n' -m 0 -c a)"
check 'ERE: unmatched ) is a literal' 'x)' "$(G 'x)\nx\n' -E 'x)')"
check 'ERE: a{1 is literal' 'a{1' "$(G 'a{1\na\n' -E 'a{1')"
check 'ERE: {,2} interval' 'aaa' "$(G 'aaa\n' -oE 'a{,2}a')"
check 'BRE: \{1\}a at the start is literal' '{1}a' "$(G '{1}a\na\n' '\{1\}a')"
check 'BRE: * after \( is literal' '*a' "$(G '*a\na\n' '\(*a\)')"
check 'BRE: $ before \) is an anchor' 'ba' "$(G 'ba\na$\n' '\(a$\)')"
check 'bracket: ] first, a- last' ']|a-' "$(G ']\na-\n' -o '[]a-]*')"
check 'bracket: [[:alpha:][:digit:]]' 'a1' "$(G 'a1-\n' -o '[[:alpha:][:digit:]]*')"
check 'bracket: equivalence and collating classes' 'ab' "$(G 'ab\n' -o '[[=a=][.b.]]*')"
check 'bracket: -i [[:upper:]] means [[:alpha:]]' 'abc' "$(G 'abc\n' -io '[[:upper:]]*')"
check '-i bracket range' 'AbC' "$(G 'AbC\n' -i '[a-c]bc')"
check 'interval {0}' 'y' "$(G 'xy\n' -oE 'x{0}y')"
check 'large interval' '1' "$(R 'x\n' -E 'a{32767}')"
check 'nested intervals' 'aaaa' "$(G 'aaaa\n' -oE '(a{2}){2}')"
check 'empty alternative' 'x' "$(G 'x\n' -E 'a||x')"
check 'empty group' 'x' "$(G 'x\n' -E '()x')"
if [ "$IS_MUSL" = 1 ]; then
    skip 'ERE back-references (musl regcomp has none)'
else
check 'back-reference' 'abab' "$(G 'abab\nabba\n' -E '(ab)\1')"
check 'back-reference with -w' 'abab' "$(G 'abab\nababx\n' -wE '(ab)\1')"
check 'back-reference mixed with plain patterns' 'abab|xyz' "$(G 'abab\nxyz\nqq\n' -E -e '(ab)\1' -e 'x.z')"
fi

echo "--- errors (exit 2, silex message) ---"
check '[ alone' "silex: grep: invalid regex '[': Invalid regular expression" "$(G 'a\n' '[')"
check 'unmatched [' "silex: grep: invalid regex '[a': Unmatched [, [^, [:, [., or [=" "$(G 'a\n' '[a')"
check 'reversed range' "silex: grep: invalid regex '[b-a]': Invalid range end" "$(G 'a\n' '[b-a]')"
check '[:alpha:] without brackets' \
    "silex: grep: invalid regex '[:alpha:]': character class syntax is [[:space:]], not [:space:]" \
    "$(G 'a\n' '[:alpha:]')"
check 'unknown class' "silex: grep: invalid regex '[[:foo:]]': Invalid character class name" "$(G 'a\n' '[[:foo:]]')"
check 'multi-char collating element' "silex: grep: invalid regex '[[.ab.]]': Invalid collation character" "$(G 'a\n' '[[.ab.]]')"
check 'trailing backslash' "silex: grep: invalid regex 'x\\': Trailing backslash" "$(G 'x\n' 'x\')"
check 'unmatched \(' "silex: grep: invalid regex '\\(a': Unmatched ( or \\(" "$(G 'a\n' '\(a')"
check 'unmatched \)' "silex: grep: invalid regex '\\)': Unmatched ) or \\)" "$(G 'a\n' '\)')"
check 'unmatched \{' "silex: grep: invalid regex 'a\\{1': Unmatched \\{" "$(G 'a\n' 'a\{1')"
check 'bad interval' "silex: grep: invalid regex 'a{2,1}': Invalid content of \\{\\}" "$(G 'a\n' -E 'a{2,1}')"
check 'interval too big' "silex: grep: invalid regex 'a{32768}': Regular expression too big" "$(G 'a\n' -E 'a{32768}')"
check 'back-reference to an open group' "silex: grep: invalid regex '\\(a\\1\\)': Invalid back reference" "$(G 'a\n' '\(a\1\)')"
check 'error exit status' '2' "$(R 'a\n' '[')"
check 'ERE (*): glibc reads the ) as a character' "silex: grep: invalid regex '(*)': Unmatched ( or \\(" "$(G 'a\n' -E '(*)')"
check 'ERE (*)): valid, and dfa.c decides the match' 'silex: grep: warning: * at start of expression|)|*)' \
    "$(G ')\na\n*)\n' -E '(*))')"
check 'warning, and the * repeats nothing' 'silex: grep: warning: * at start of expression|a' "$(G 'a\n' -E '*a')"
check 'leading {1} applies to nothing' 'silex: grep: warning: {...} at start of expression|a' "$(G 'a\n' -E '{1}a')"

echo "--- -o and --color read the pattern the way glibc does (GNU) ---"
check '-o {1}a prints 1}a' '1}a' "$("$SILEX" grep -oE '{1}a' <<< '1}a' 2>/dev/null)"
check '-o c\>* has a literal *' 'c*' "$(G 'c*\n' -o 'c\>*')"
check '-x pastes the pattern into ^(...)$ as text' 'A)' "$(G 'A)\nA\n' -xE ')?.')"
check '-w pastes the pattern as text too' 'x)' "$(G 'x)\nx\n' -wE 'x)')"
check 'pasted patterns: an early ) ends the group, \n is an alternative' '3' \
    "$(G 'ab ba\nabx\n\n' -wcE -e ')' -e 'b+')"
check 'only plain strings: no pasting (GNU uses its -F matcher)' ')' \
    "$(G 'a\n)\n\n' -xE -e ')' -e 'q')"

echo "--- input handling ---"
check 'unterminated last line, ^$' '0' "$(G 'xxxxo' -c '^$')"
check 'unterminated last line gets a newline' 'ab|cd' "$(G 'ab\ncd' '[a-d]')"
check 'stdin is "(standard input)" for -H' '(standard input):a' "$(printf 'a\n' | "$SILEX" grep -H a)"
check 'stdin is "(standard input)" for -l' '(standard input)' "$(printf 'a\n' | "$SILEX" grep -l a)"
check 'stdin is listed by -L' '(standard input)' "$(printf 'a\n' | "$SILEX" grep -L b)"
check 'directory as stdin' 'silex: grep: (standard input): Is a directory|2' \
    "$({ "$SILEX" grep a < / 2>&1; echo $?; } | paste -sd'|' -)"
printf 'a1\nb\na2\nc\n' > "$T/s"
check '-m1 leaves stdin after the match' 'a1|b|a2|c' "$("$SILEX" sh -c "{ grep -m1 a; cat; } < '$T/s'" | paste -sd'|' -)"
check '-q leaves stdin after the match' 'b|a2|c' "$("$SILEX" sh -c "{ grep -q a; cat; } < '$T/s'" | paste -sd'|' -)"
check 'standalone -m1 leaves the offset too' 'a1|b|a2|c' "$({ "$SILEX" grep -m1 a; cat; } < "$T/s" | paste -sd'|' -)"
printf 'foo\n\0bar\n' > "$T/bin"
check 'a text line matched before a NUL still exits 0' 'foo|0' "$({ "$SILEX" grep foo "$T/bin"; echo $?; } | paste -sd'|' -)"

echo "--- output to /dev/null stops at the first selected line ---"
printf 'x\nx\n' > "$T/x"
check 'exit 0 on a match' '0' "$("$SILEX" grep x "$T/x" > /dev/null; echo $?)"
check 'exit 1 without' '1' "$("$SILEX" grep y "$T/x" > /dev/null; echo $?)"
check 'later files are still opened' '2' "$("$SILEX" grep x "$T/x" "$T/missing" > /dev/null 2>&1; echo $?)"
check '-v' '0' "$("$SILEX" grep -v y "$T/x" > /dev/null; echo $?)"
check '-L, file not listed' '0' "$("$SILEX" grep -L x "$T/x" > /dev/null; echo $?)"
check '-L, file listed' '1' "$("$SILEX" grep -L y "$T/x" > /dev/null; echo $?)"

echo "--- block scanner ---"
# Lines on both sides of the 32 KB and 256 KB read boundaries, a line
# longer than any read, and a match on the last line of each block.
awk 'BEGIN { for (i = 1; i <= 60000; i++) { if (i % 1000 == 0) print "line " i " needle here"; else print "line " i " filler text" } }' > "$T/big"
awk 'BEGIN { s = ""; for (i = 0; i < 70000; i++) s = s "abcdefghij"; print s "needle"; print "short needle"; print s }' > "$T/long"
check 'count across reads' '60' "$("$SILEX" grep -c needle "$T/big")"
check 'regex count across reads' '60' "$("$SILEX" grep -cE 'ne+dle h' "$T/big")"
check '-n across reads' '1000:line 1000 needle here|60000:line 60000 needle here' \
    "$("$SILEX" grep -n needle "$T/big" | sed -n '1p;$p' | paste -sd'|' -)"
if [ "$GNU_GREP" = 1 ]; then
check '-b across reads' "$(grep -b needle "$T/big" | tail -1 | cut -d: -f1)" \
    "$("$SILEX" grep -b needle "$T/big" | tail -1 | cut -d: -f1)"
else
    skip '-b across reads (needs GNU grep as the reference)'
fi
check '-v count across reads' '59940' "$("$SILEX" grep -vc needle "$T/big")"
check '-v output across reads' "$(grep -v needle "$T/big" | cksum)" "$("$SILEX" grep -v needle "$T/big" | cksum)"
check 'a 700 KB line' '2' "$("$SILEX" grep -c needle "$T/long")"
check 'a 700 KB line, regex' '2' "$("$SILEX" grep -c 'j*needle$' "$T/long")"
check 'a 700 KB line, -o' 'needle|needle' "$("$SILEX" grep -o needle "$T/long" | paste -sd'|' -)"
check 'stdin through a pipe' '60' "$(cat "$T/big" | "$SILEX" grep -c needle)"
check 'start-state skip: pattern starting with a class' '60' "$("$SILEX" grep -cE '[n]e{2}dle' "$T/big")"
check 'dead-state skip: anchored pattern' '60' "$("$SILEX" grep -c '^line [0-9]*000 ' "$T/big")"
check 'patterns that match every line: -c' '60000' "$("$SILEX" grep -c 'x*' "$T/big")"
check 'patterns that match every line: output' "$(cksum < "$T/big")" "$("$SILEX" grep '' "$T/big" | cksum)"
check 'patterns that match every line: -v' '0' "$("$SILEX" grep -vc 'q*' "$T/big")"
check 'alternation of literals' '60' "$("$SILEX" grep -cE 'zzz|needle|qqq' "$T/big")"
check '-i literal' '60' "$("$SILEX" grep -ci NEEDLE "$T/big")"
check 'dense literal hits (prefilter gives up)' '60' "$("$SILEX" grep -c 'l.*needle' "$T/big")"
check 'one-byte literals' '60000' "$("$SILEX" grep -cE 'e|i' "$T/big")"
check 'DFA cache flush (exponential pattern)' \
    "$(head -c 300000 /dev/zero | tr '\0' 'a' | fold -w 50 | awk 'NR%3==0{print "b" $0; next} {print}' | grep -cE '(a|b)*b(a|b){14}')" \
    "$(head -c 300000 /dev/zero | tr '\0' 'a' | fold -w 50 | awk 'NR%3==0{print "b" $0; next} {print}' | "$SILEX" grep -cE '(a|b)*b(a|b){14}')"

echo ""
echo "grep engine: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
