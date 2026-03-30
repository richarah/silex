#!/bin/sh
# check.sh — matchbox quality audit script
# Usage: sh check.sh
# Exits 0 if all checks pass, non-zero otherwise.

set -e

BINARY=build/bin/matchbox
PASS=0
FAIL=0
WARN=0

ok()   { echo "  PASS: $*"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL+1)); }
warn() { echo "  WARN: $*"; WARN=$((WARN+1)); }

header() { echo; echo "=== $* ==="; }

# ---------------------------------------------------------------------------
# 1. Build
# ---------------------------------------------------------------------------
header "Build"
if make 2>&1 | tail -3; then
    ok "make succeeded"
else
    fail "make failed"
    exit 1
fi

if [ -x "$BINARY" ]; then
    ok "binary exists: $BINARY"
else
    fail "binary missing: $BINARY"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Binary size
# ---------------------------------------------------------------------------
header "Binary size"
SIZE=$(wc -c < "$BINARY")
SIZE_KB=$((SIZE / 1024))
echo "  size: ${SIZE_KB}K (${SIZE} bytes)"
if [ "$SIZE" -le 2097152 ]; then   # 2 MB
    ok "binary size within 2 MB limit"
else
    fail "binary exceeds 2 MB limit (${SIZE_KB}K)"
fi

# ---------------------------------------------------------------------------
# 3. Version string
# ---------------------------------------------------------------------------
header "Version"
VERSION=$("$BINARY" --version 2>&1 | head -1)
echo "  $VERSION"
if echo "$VERSION" | grep -q "^matchbox"; then
    ok "--version output looks correct"
else
    fail "--version output unexpected: $VERSION"
fi

# ---------------------------------------------------------------------------
# 4. Builtin inventory
# ---------------------------------------------------------------------------
header "Builtin inventory"
REQUIRED="basename cat chmod cp cut date dirname echo find grep head install \
          ln mkdir mv printf readlink rm sed sort stat tail touch tr wc xargs sh \
          mktemp tee env realpath sha256sum"
MISSING=""
for b in $REQUIRED; do
    if ! "$BINARY" --list 2>/dev/null | grep -qw "$b" && \
       ! "$BINARY" -c "$b --help" >/dev/null 2>&1 && \
       ! "$BINARY" -c "type $b" 2>/dev/null | grep -q builtin; then
        MISSING="$MISSING $b"
    fi
done
if [ -z "$MISSING" ]; then
    ok "all required builtins present"
else
    warn "builtins not confirmed:$MISSING"
fi

# ---------------------------------------------------------------------------
# 5. Unit tests
# ---------------------------------------------------------------------------
header "Unit tests"
if make test 2>&1 | grep -E "^(PASS|FAIL|Results)" | tail -5; then
    if make test 2>&1 | grep -q "^FAIL"; then
        fail "unit tests have failures"
    else
        ok "unit tests all pass"
    fi
fi

# ---------------------------------------------------------------------------
# 6. Compat tests
# ---------------------------------------------------------------------------
header "Compat tests (TAP)"
COMPAT_OUT=$(make compat-test 2>&1)
echo "$COMPAT_OUT" | tail -3
if echo "$COMPAT_OUT" | grep -q "0 failed"; then
    ok "compat tests all pass"
else
    fail "compat tests have failures"
fi

# ---------------------------------------------------------------------------
# 7. Shell conformance tests
# ---------------------------------------------------------------------------
header "Shell conformance tests"
SHELL_PASS=0
SHELL_FAIL=0
for f in tests/unit/shell/test_*.sh; do
    OUT=$(sh "$f" "$BINARY" 2>&1)
    P=$(echo "$OUT" | grep -c "^PASS" || true)
    F=$(echo "$OUT" | grep -c "^FAIL" || true)
    SHELL_PASS=$((SHELL_PASS + P))
    SHELL_FAIL=$((SHELL_FAIL + F))
    if [ "$F" -gt 0 ]; then
        echo "  FAIL in $f:"
        echo "$OUT" | grep "^FAIL" | sed 's/^/    /'
    fi
done
echo "  shell conformance: ${SHELL_PASS} passed, ${SHELL_FAIL} failed"
if [ "$SHELL_FAIL" -eq 0 ]; then
    ok "shell conformance: all pass"
else
    fail "shell conformance: ${SHELL_FAIL} failures"
fi

# ---------------------------------------------------------------------------
# 8. Integration tests
# ---------------------------------------------------------------------------
header "Integration tests"
INTEG_OUT=$(make integration-test 2>&1)
echo "$INTEG_OUT" | tail -3
if echo "$INTEG_OUT" | grep -qE "^(FAIL:|not ok |\# .* [1-9][0-9]* failed)"; then
    fail "integration tests have failures"
else
    ok "integration tests pass"
fi

# ---------------------------------------------------------------------------
# 9. Security tests
# ---------------------------------------------------------------------------
header "Security tests"
SEC_OUT=$(make security-test 2>&1)
echo "$SEC_OUT" | tail -3
if echo "$SEC_OUT" | grep -qE "^(FAIL:|not ok |\# .* [1-9][0-9]* failed)"; then
    fail "security tests have failures"
else
    ok "security tests pass"
fi

# ---------------------------------------------------------------------------
# 10. Static analysis (if cppcheck available)
# ---------------------------------------------------------------------------
header "Static analysis"
if command -v cppcheck >/dev/null 2>&1; then
    CPPCHECK_OUT=$(cppcheck --error-exitcode=1 --quiet \
        --suppress=missingIncludeSystem \
        -I src src/ 2>&1)
    if [ $? -eq 0 ]; then
        ok "cppcheck: no errors"
    else
        echo "$CPPCHECK_OUT" | head -10
        fail "cppcheck found errors"
    fi
else
    warn "cppcheck not available (skipping)"
fi

# ---------------------------------------------------------------------------
# 11. Shellcheck (if available)
# ---------------------------------------------------------------------------
header "Shellcheck"
if command -v shellcheck >/dev/null 2>&1; then
    SC_FAIL=0
    for f in tests/unit/shell/*.sh tests/compat/run.sh check.sh; do
        [ -f "$f" ] || continue
        if ! shellcheck -S warning "$f" >/dev/null 2>&1; then
            warn "shellcheck warnings in $f"
            SC_FAIL=$((SC_FAIL + 1))
        fi
    done
    if [ "$SC_FAIL" -eq 0 ]; then
        ok "shellcheck: no warnings"
    fi
else
    warn "shellcheck not available (skipping)"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "========================================="
echo " check.sh summary"
echo "========================================="
echo "  PASS: $PASS"
echo "  WARN: $WARN"
echo "  FAIL: $FAIL"
echo "========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAIL ($FAIL failures)"
    exit 1
fi
echo "RESULT: PASS"
exit 0
