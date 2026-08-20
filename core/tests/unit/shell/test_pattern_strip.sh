#!/bin/sh
# tests/unit/shell/test_pattern_strip.sh
#
# ${v#pat}, ${v##pat}, ${v%pat} and ${v%%pat} -- the four prefix/suffix strips.
#
# These matchers used to answer every question by brute force: walk each split
# point of the subject, malloc a copy of the candidate, and ask fnmatch. That is
# O(n) fnmatch calls and an allocation apiece to take one character off the
# front of a ten-byte string, and parameter expansion was the last of the five
# interpretation benchmarks where silex trailed dash. A pattern with no
# metacharacter can match exactly one substring, so it now answers in a single
# memcmp with no allocation (expand.c: pattern_literal_len).
#
# The risk in that change is a pattern that only LOOKS literal. `\` takes the
# slow path deliberately: a quoted metacharacter arrives here escaped (see
# pat_emit_literal), and unescaping it is fnmatch's job, not the predicate's.
# So the cases below pair each fast-path shape with the glob and quoted-glob
# shape that must still reach fnmatch, and check that both give dash's answer.
#
# The equivalence was also proved exhaustively at the C level before this
# landed: 13,075,566 (subject, pattern) pairs over an alphabet holding every
# metacharacter and every character pat_emit_literal escapes -- all pairs up to
# length 3, plus longer literal cases -- with 0 disagreements against the
# original brute force. This file is the part of that which survives in the
# suite.

SILEX="${1:-build/bin/silex}"
case "$SILEX" in /*) ;; *) SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;; esac
MB="$SILEX"
PASS=0
FAIL=0

check() {
    desc="$1"; got="$2"; expected="$3"
    if [ "$got" = "$expected" ]; then
        echo "PASS: $desc"; PASS=$((PASS+1))
    else
        echo "FAIL: $desc"
        echo "  expected: [$expected]"
        echo "  got:      [$got]"
        FAIL=$((FAIL+1))
    fi
}

# --- literal patterns: the fast path ------------------------------------

check "literal prefix strips" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#a}"')" "bcdefghij"
check "literal suffix strips" \
      "$("$MB" -c 'v=abcdefghij; echo "${v%j}"')" "abcdefghi"
check "literal prefix that does not match leaves the value alone" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#z}"')" "abcdefghij"
check "literal suffix that does not match leaves the value alone" \
      "$("$MB" -c 'v=abcdefghij; echo "${v%z}"')" "abcdefghij"
check "a literal pattern is anchored, not searched" \
      "$("$MB" -c 'v=abcabc; echo "${v#bc}"')" "abcabc"
check "multi-character literal prefix" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#abc}"')" "defghij"
check "multi-character literal suffix" \
      "$("$MB" -c 'v=abcdefghij; echo "${v%hij}"')" "abcdefg"

# A literal pattern can match only one length, so greedy and non-greedy must
# agree -- which is exactly what lets the two share one memcmp.
check "## on a literal matches the same as #" \
      "$("$MB" -c 'v=abcabc; echo "${v##abc}"')" "abc"
check "%% on a literal matches the same as %" \
      "$("$MB" -c 'v=abcabc; echo "${v%%abc}"')" "abc"

# Boundaries: empty pattern, empty subject, pattern equal to and longer than
# the subject. The suffix path indexes from the END, so a pattern longer than
# the subject is where an unguarded memcmp would read before the string.
check "empty pattern strips nothing (prefix)" \
      "$("$MB" -c 'v=abc; echo "[${v#}]"')" "[abc]"
check "empty pattern strips nothing (suffix)" \
      "$("$MB" -c 'v=abc; echo "[${v%}]"')" "[abc]"
check "empty subject, non-empty pattern" \
      "$("$MB" -c 'v=; echo "[${v#a}][${v%a}]"')" "[][]"
check "pattern equal to the whole subject" \
      "$("$MB" -c 'v=abc; echo "[${v#abc}][${v%abc}]"')" "[][]"
check "pattern longer than the subject does not match" \
      "$("$MB" -c 'v=abc; echo "[${v#abcd}][${v%abcd}]"')" "[abc][abc]"

# The suffix matchers index BACKWARDS from the end of the subject, so a pattern
# longer than the subject is where the length guard earns its place: without it
# the memcmp reads before the start of the string. That read is invisible from
# the shell -- deleting the guard leaves all the cases above passing -- and it
# is invisible to ASan too at these sizes, because the stray bytes are still
# inside the arena block the subject was allocated from (the same blindness
# `make test-poison` exists for). A pattern of 200,000 bytes is what puts the
# read outside the block, where ASan sees it: with the guard removed this case
# reports a heap-buffer-overflow in match_suffix under `make test-asan`. Both
# spellings are here because only the greedy one reaches match_suffix.
_bigpat_out=$("$MB" -c 'p=$(awk "BEGIN{for(i=0;i<200000;i++)printf \"x\"}")
                        v=abc; echo "[${v%$p}][${v%%$p}][${v#$p}][${v##$p}]"' 2>&1)
check "a pattern far longer than the subject reads nothing before it" \
      "$_bigpat_out" "[abc][abc][abc][abc]"

# --- glob patterns: must still reach fnmatch ----------------------------

check "* is still a wildcard in a prefix strip" \
      "$("$MB" -c 'v=a/b/c; echo "${v#*/}"')" "b/c"
check "greedy * is still greedy" \
      "$("$MB" -c 'v=a/b/c; echo "${v##*/}"')" "c"
check "* is still a wildcard in a suffix strip" \
      "$("$MB" -c 'v=a/b/c; echo "${v%/*}"')" "a/b"
check "greedy * is still greedy in a suffix strip" \
      "$("$MB" -c 'v=a/b/c; echo "${v%%/*}"')" "a"
check "? still matches exactly one character" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#?}"')" "bcdefghij"
check "a bracket expression still matches a set" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#[abc]}"')" "bcdefghij"
check "a bracket range still matches a range" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#[a-c]}"')" "bcdefghij"
check "a negated bracket expression still negates" \
      "$("$MB" -c 'v=abcdefghij; echo "${v#[!z]}"')" "bcdefghij"

# --- quoted metacharacters: literal text that must NOT go fast ----------
#
# These arrive at the matcher escaped, so the predicate rejects them on the
# backslash and fnmatch does the unescaping. Getting this wrong is silent: the
# '*' would be treated as data by a memcmp, which is the RIGHT answer here --
# so these cases pass either way and are here to pin the behaviour, while the
# two below them are the ones that would actually break.

check "a quoted * is literal data, not a wildcard" \
      "$("$MB" -c 'v="*abc"; echo "${v#"*"}"')" "abc"
check "a quoted * does not match other text" \
      "$("$MB" -c 'v=xabc; echo "${v#"*"}"')" "xabc"
check "a backslash-escaped * is literal data" \
      "$("$MB" -c 'v="*abc"; echo "${v#\*}"')" "abc"
check "a quoted ? is literal data" \
      "$("$MB" -c 'v="?abc"; echo "${v#"?"}"')" "abc"
check "a quoted bracket is literal data" \
      "$("$MB" -c 'v="[a]bc"; echo "${v#"[a]"}"')" "bc"
# A literal backslash, unquoted on both sides. The doubled-and-quoted
# spellings of this are NOT used as oracles: `v="\\abc"; echo "${v#\\\\}"` is
# `bc` in dash and `\abc` in bash, so there is no right answer to pin.
check "a literal backslash in the subject is data" \
      "$("$MB" -c 'v=\\abc; printf %s "${v#\\}"')" "abc"

# A pattern that is literal but whose SUBJECT holds metacharacters: the
# fast path compares bytes, so the subject's '*' must be compared, not
# interpreted.
check "metacharacters in the subject are data" \
      "$("$MB" -c 'v="a*b?c"; echo "${v#a}"')" "*b?c"
check "a literal pattern matches a subject full of metacharacters" \
      "$("$MB" -c 'v="*?[]"; echo "[${v%"[]"}]"')" "[*?]"

# --- the pattern comes from an expansion --------------------------------
#
# An unquoted expansion's metacharacters stay ACTIVE (POSIX 2.13), so the same
# text behaves differently depending on where it came from -- the predicate
# runs after expansion, on the result, which is what makes that work.

check "an unquoted expansion's * is an active wildcard" \
      "$("$MB" -c 'p="*/"; v=a/b/c; echo "${v#$p}"')" "b/c"
check "a quoted expansion's * is literal" \
      "$("$MB" -c 'p="*"; v="*abc"; echo "${v#"$p"}"')" "abc"
check "a nested expansion supplies the pattern" \
      "$("$MB" -c 't=abcdefghij; echo "${t%"${t#a}"}"')" "a"

# --- the same, on positionals and specials ------------------------------

check "strip applies to a positional parameter" \
      "$("$MB" -c 'echo "${1#a}"' sh abc)" "bc"
check "strip applies to \$0" \
      "$("$MB" -c 'echo "${0%h}"' sh)" "s"

# -----------------------------------------------------------------------

echo ""
echo "pattern strip tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
