#!/bin/bash
# test_mkdir.sh — unit tests for the mkdir builtin
# Usage: test_mkdir.sh [path/to/silex]

set -euo pipefail

MB="${1:-build/bin/silex}"
MKDIR="$MB mkdir"

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

check_ok() {
    local desc="$1"
    if eval "$2"; then
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected success)"
        FAIL=$((FAIL + 1))
    fi
}

check_fail() {
    local desc="$1"
    if eval "$2"; then
        echo "  FAIL: $desc (expected failure)"
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
}

# --- Basic mkdir ---
D="$TMPDIR_LOCAL/basic"
check_ok "basic mkdir" "$MKDIR '$D'"
check "basic mkdir: dir exists" "$([ -d "$D" ] && echo yes)" "yes"

# --- mkdir -p (parents) ---
D="$TMPDIR_LOCAL/a/b/c"
check_ok "mkdir -p deep" "$MKDIR -p '$D'"
check "mkdir -p: leaf exists" "$([ -d "$D" ] && echo yes)" "yes"

# --- mkdir -p on existing directory should succeed ---
check_ok "mkdir -p existing" "$MKDIR -p '$D'"

# --- mkdir -p multiple dirs ---
D1="$TMPDIR_LOCAL/d1/e1"
D2="$TMPDIR_LOCAL/d2/e2"
check_ok "mkdir -p multiple" "$MKDIR -p '$D1' '$D2'"
check "mkdir -p multi: d1" "$([ -d "$D1" ] && echo yes)" "yes"
check "mkdir -p multi: d2" "$([ -d "$D2" ] && echo yes)" "yes"

# --- mkdir -v (verbose) ---
D="$TMPDIR_LOCAL/verbose"
OUT=$($MKDIR -v "$D" 2>&1)
check "mkdir -v output format" "$OUT" "mkdir: created directory '$D'"

# --- mkdir -v -p ---
D="$TMPDIR_LOCAL/vp/x"
OUT=$($MKDIR -vp "$D" 2>&1)
# Should print at least the leaf directory
check "mkdir -vp prints leaf" "$(echo "$OUT" | grep -c "created directory")" "2"

# --- mkdir without -p on existing dir should fail ---
D="$TMPDIR_LOCAL/exists"
mkdir -p "$D"
check_fail "mkdir existing without -p" "$MKDIR '$D' 2>/dev/null"

# --- mkdir -m mode ---
D="$TMPDIR_LOCAL/moded"
$MKDIR -m 0700 "$D"
PERM=$(stat -c '%a' "$D")
check "mkdir -m 0700" "$PERM" "700"

D2="$TMPDIR_LOCAL/moded2"
$MKDIR -m 0755 "$D2"
PERM2=$(stat -c '%a' "$D2")
check "mkdir -m 0755" "$PERM2" "755"

# --- mkdir no args should fail ---
check_fail "mkdir no args" "$MKDIR 2>/dev/null"

# --- path with spaces ---
D="$TMPDIR_LOCAL/path with spaces"
check_ok "mkdir path with spaces" "$MKDIR -p '$D'"
check "mkdir spaces: dir exists" "$([ -d "$D" ] && echo yes)" "yes"

# --- Error on non-directory parent ---
F="$TMPDIR_LOCAL/isfile"
touch "$F"
check_fail "mkdir child of file" "$MKDIR -p '$F/subdir' 2>/dev/null"

echo "  mkdir: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
