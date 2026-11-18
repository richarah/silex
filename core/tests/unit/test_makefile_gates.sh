#!/bin/sh
# tests/unit/test_makefile_gates.sh — verify Makefile loop targets propagate failures
# Ensures shell-test, security-test, and fuzz-run fail if ANY script/fuzzer fails,
# even when the last one in lexicographic order succeeds.

PASS=0
FAIL=0

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
CORE_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"

if ! command -v make >/dev/null 2>&1; then
    echo "SKIP: make not available"
    exit 0
fi

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

cp "$CORE_DIR/Makefile" "$W/Makefile"
mkdir -p "$W/build/bin" "$W/build/fuzz" \
         "$W/tests/unit/shell" "$W/tests/security"

printf '#!/bin/sh\nexit 0\n' > "$W/build/bin/silex"
chmod +x "$W/build/bin/silex"

expect_rc() {
    desc="$1"
    want_nonzero="$2"
    got="$3"
    if [ "$want_nonzero" = "1" ]; then
        if [ "$got" -ne 0 ]; then
            echo "PASS: $desc"
            PASS=$((PASS + 1))
        else
            echo "FAIL: $desc (expected non-zero exit, got 0)"
            FAIL=$((FAIL + 1))
        fi
    else
        if [ "$got" -eq 0 ]; then
            echo "PASS: $desc"
            PASS=$((PASS + 1))
        else
            echo "FAIL: $desc (expected exit 0, got $got)"
            FAIL=$((FAIL + 1))
        fi
    fi
}

# 1. shell-test: first script fails, last script succeeds -> must fail
printf '#!/bin/sh\nexit 1\n' > "$W/tests/unit/shell/test_01_fail.sh"
printf '#!/bin/sh\nexit 0\n' > "$W/tests/unit/shell/test_02_pass.sh"
make -C "$W" -o build/bin/silex shell-test >/dev/null 2>&1
expect_rc "Makefile shell-test fails when non-last script fails" 1 "$?"

printf '#!/bin/sh\nexit 0\n' > "$W/tests/unit/shell/test_01_fail.sh"
make -C "$W" -o build/bin/silex shell-test >/dev/null 2>&1
expect_rc "Makefile shell-test succeeds when all scripts pass" 0 "$?"

# 2. security-test: first script fails, last script succeeds -> must fail
printf '#!/bin/sh\nexit 1\n' > "$W/tests/security/test_01_fail.sh"
printf '#!/bin/sh\nexit 0\n' > "$W/tests/security/test_02_pass.sh"
make -C "$W" -o build/bin/silex security-test >/dev/null 2>&1
expect_rc "Makefile security-test fails when non-last script fails" 1 "$?"

printf '#!/bin/sh\nexit 0\n' > "$W/tests/security/test_01_fail.sh"
make -C "$W" -o build/bin/silex security-test >/dev/null 2>&1
expect_rc "Makefile security-test succeeds when all scripts pass" 0 "$?"

# 3. fuzz-run: first fuzzer fails, last fuzzer succeeds -> must fail
printf '#!/bin/sh\nexit 1\n' > "$W/build/fuzz/fuzz_01_fail"
printf '#!/bin/sh\nexit 0\n' > "$W/build/fuzz/fuzz_02_pass"
chmod +x "$W/build/fuzz/fuzz_01_fail" "$W/build/fuzz/fuzz_02_pass"
make -C "$W" -o fuzz -o build/fuzz/fuzz_shell_lexer -o build/fuzz/fuzz_shell_parser \
    -o build/fuzz/fuzz_path_canon -o build/fuzz/fuzz_grep_pattern -o build/fuzz/fuzz_sed_expr \
    fuzz-run >/dev/null 2>&1
expect_rc "Makefile fuzz-run fails when non-last fuzzer fails" 1 "$?"

printf '#!/bin/sh\nexit 0\n' > "$W/build/fuzz/fuzz_01_fail"
chmod +x "$W/build/fuzz/fuzz_01_fail"
make -C "$W" -o fuzz -o build/fuzz/fuzz_shell_lexer -o build/fuzz/fuzz_shell_parser \
    -o build/fuzz/fuzz_path_canon -o build/fuzz/fuzz_grep_pattern -o build/fuzz/fuzz_sed_expr \
    fuzz-run >/dev/null 2>&1
expect_rc "Makefile fuzz-run succeeds when all fuzzers pass" 0 "$?"

echo ""
echo "makefile_gates: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
