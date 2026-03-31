#!/bin/bash
# test_echo.sh — unit tests for the echo builtin
# Usage: test_echo.sh [path/to/silex]

set -euo pipefail

MB="${1:-build/bin/silex}"
ECHO="$MB echo"
GNU_ECHO="/bin/echo"

PASS=0
FAIL=0
TMPDIR_LOCAL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

check() {
    local desc="$1"
    local got="$2"
    local expected="$3"
    if [ "$got" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    got:      $(printf '%q' "$got")"
        echo "    expected: $(printf '%q' "$expected")"
        FAIL=$((FAIL + 1))
    fi
}

check_against_gnu() {
    local desc="$1"
    shift
    local mb_out gnu_out
    mb_out=$($ECHO "$@" 2>&1 || true)
    gnu_out=$($GNU_ECHO "$@" 2>&1 || true)
    check "gnu-compat: $desc" "$mb_out" "$gnu_out"
}

# --- Basic output ---
check "no args"        "$($ECHO)"              ""
check "one arg"        "$($ECHO hello)"        "hello"
check "two args"       "$($ECHO hello world)"  "hello world"
check "empty string"   "$($ECHO '')"           ""
check "special chars"  "$($ECHO 'a&b|c')"      "a&b|c"

# --- -n flag (suppress newline) ---
check "-n suppresses newline"  "$(printf '%s' "$($ECHO -n hello)")"  "hello"
check "-n no args"             "$(printf '%s' "$($ECHO -n)")"        ""

# --- -e flag (escape interpretation) ---
check "-e newline"       "$($ECHO -e 'a\nb')"    "$(printf 'a\nb')"
check "-e tab"           "$($ECHO -e 'a\tb')"    "$(printf 'a\tb')"
check "-e backslash"     "$($ECHO -e 'a\\\\b')"  "$(printf 'a\\\\b')"
check "-e bell"          "$($ECHO -e '\a')"       "$(printf '\a')"
check "-e hex"           "$($ECHO -e '\x41')"     "A"
check "-e octal"         "$($ECHO -e '\101')"     "A"
check "-e \\c stops"     "$($ECHO -e 'ab\ccd')"  "ab"

# --- -E flag (disable escapes, default) ---
check "-E no escapes"    "$($ECHO -E 'a\nb')"    'a\nb'
check "no flag no esc"   "$($ECHO 'a\nb')"       'a\nb'

# --- Flag combinations ---
check "-ne combo"    "$(printf '%s' "$($ECHO -ne 'a\nb')")"  "$(printf 'a\nb')"
check "-en combo"    "$(printf '%s' "$($ECHO -en 'a\nb')")"  "$(printf 'a\nb')"
check "-nE combo"    "$(printf '%s' "$($ECHO -nE 'hello')")" "hello"

# --- Argument that looks like flag but isn't ---
check "double dash passthrough"  "$($ECHO -- hello)"  "-- hello"
check "-z not a flag"            "$($ECHO -z hello)"  "-z hello"

# --- GNU compatibility ---
check_against_gnu "basic hello"   "hello"
check_against_gnu "two words"     "foo" "bar"
check_against_gnu "-n"            -n "hello"
check_against_gnu "-e newline"    -e "a\nb"
check_against_gnu "-e tab"        -e "a\tb"
check_against_gnu "empty"

echo "  echo: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
