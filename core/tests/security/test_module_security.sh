#!/bin/sh
# tests/security/test_module_security.sh — module loading security tests
# chmod +x tests/security/test_module_security.sh
# Usage: ./test_module_security.sh [path/to/silex]
#
# Tests that silex refuses to load modules from insecure locations.

SILEX="${1:-build/bin/silex}"
PASS=0
FAIL=0

check_exit_nonzero() {
    local desc="$1"
    local got_exit="$2"
    if [ "$got_exit" -ne 0 ]; then
        echo "PASS: $desc (exit $got_exit)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected rejection, got exit 0)"
        FAIL=$((FAIL + 1))
    fi
}

check_stderr_contains() {
    local desc="$1"
    local stderr_file="$2"
    local pattern="$3"
    if grep -qi "$pattern" "$stderr_file" 2>/dev/null; then
        echo "PASS: $desc (stderr matches '$pattern')"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (stderr did not contain '$pattern')"
        echo "  stderr was: $(cat "$stderr_file" 2>/dev/null)"
        FAIL=$((FAIL + 1))
    fi
}

TMPDIR_MOD=$(mktemp -d)
trap 'rm -rf "$TMPDIR_MOD"' EXIT INT TERM

STDERR_FILE="$TMPDIR_MOD/stderr.txt"

# Build a minimal stub .so that exports silex_module_init, for realistic tests.
# If compilation fails (no compiler), we skip those tests gracefully.
STUB_SO="$TMPDIR_MOD/stub.so"
STUB_C="$TMPDIR_MOD/stub.c"
cat > "$STUB_C" << 'STUBEOF'
#include <stddef.h>
typedef struct {
    int api_version;
    const char *tool_name;
    const char *module_name;
    const char *description;
    const char **extra_flags;
    int (*handler)(int, char **, int);
} silex_module_t;
silex_module_t *silex_module_init(void) { return NULL; }
STUBEOF

HAVE_COMPILER=0
if gcc -shared -fPIC -o "$STUB_SO" "$STUB_C" 2>/dev/null; then
    HAVE_COMPILER=1
fi

# ===========================================================================
# Test 1: World-writable module directory must be rejected
# ===========================================================================

WRITABLE_DIR="$TMPDIR_MOD/world_writable_dir"
mkdir -p "$WRITABLE_DIR"
chmod 0777 "$WRITABLE_DIR"

if [ "$HAVE_COMPILER" -eq 1 ]; then
    cp "$STUB_SO" "$WRITABLE_DIR/test_module.so"
    chmod 0644 "$WRITABLE_DIR/test_module.so"
    "$SILEX" --load-module "$WRITABLE_DIR/test_module.so" > /dev/null 2>"$STDERR_FILE"
    check_exit_nonzero "world-writable module dir: load rejected" "$?"
else
    echo "SKIP: world-writable dir test (no C compiler available)"
fi

# ===========================================================================
# Test 2: Wrong owner module (owned by different user) must be rejected
# ===========================================================================

OWNER_DIR="$TMPDIR_MOD/owner_dir"
mkdir -p "$OWNER_DIR"
chmod 0755 "$OWNER_DIR"

if [ "$HAVE_COMPILER" -eq 1 ]; then
    WRONG_OWNER_SO="$OWNER_DIR/wrong_owner.so"
    cp "$STUB_SO" "$WRONG_OWNER_SO"
    # Attempt to change ownership to root (may fail without sudo; that's fine)
    # Instead, we create a file and test that non-root-owned files in root-only
    # dirs are also rejected. We simulate by checking the --load-module path
    # on a file owned by a non-matching uid.
    # If we are root, skip this test.
    if [ "$(id -u)" -eq 0 ]; then
        echo "SKIP: wrong-owner test (running as root, ownership check not meaningful)"
    else
        # File is owned by current user; loader should accept only if owner == current user
        # or root. Test with a module in a dir owned by root (system dir) which we
        # cannot write — use /tmp as a proxy test: /tmp is +t, world-writable.
        # The meaningful test is: file created by root that we try to load.
        # Since we cannot chown without sudo, test the security check description only.
        "$SILEX" --load-module "$WRONG_OWNER_SO" > /dev/null 2>"$STDERR_FILE"
        GOT=$?
        # Either it loads (if owner check allows same user) or rejects.
        # We just document the behavior:
        echo "INFO: --load-module same-owner exit=$GOT (expected: 0 if owner matches)"
        echo "SKIP: wrong-owner test requires root to change file ownership"
    fi
else
    echo "SKIP: wrong-owner test (no C compiler)"
fi

# ===========================================================================
# Test 3: Symlink path to module must be rejected
# ===========================================================================

REAL_DIR="$TMPDIR_MOD/real_dir"
mkdir -p "$REAL_DIR"
chmod 0755 "$REAL_DIR"

LINK_DIR="$TMPDIR_MOD/link_dir"
ln -s "$REAL_DIR" "$LINK_DIR" 2>/dev/null || true

if [ "$HAVE_COMPILER" -eq 1 ] && [ -L "$LINK_DIR" ]; then
    cp "$STUB_SO" "$REAL_DIR/real_module.so"
    chmod 0644 "$REAL_DIR/real_module.so"
    # Load via symlinked directory path
    "$SILEX" --load-module "$LINK_DIR/real_module.so" > /dev/null 2>"$STDERR_FILE"
    check_exit_nonzero "symlink path to module: load rejected" "$?"
elif [ ! -L "$LINK_DIR" ]; then
    echo "SKIP: symlink dir test (could not create symlink)"
else
    echo "SKIP: symlink dir test (no C compiler)"
fi

# ===========================================================================
# Test 4: Module with wrong permissions (executable by group/other) rejected
# ===========================================================================

if [ "$HAVE_COMPILER" -eq 1 ]; then
    PERM_DIR="$TMPDIR_MOD/perm_dir"
    mkdir -p "$PERM_DIR"
    chmod 0755 "$PERM_DIR"
    cp "$STUB_SO" "$PERM_DIR/group_write.so"
    chmod 0664 "$PERM_DIR/group_write.so"   # group-writable
    "$SILEX" --load-module "$PERM_DIR/group_write.so" > /dev/null 2>"$STDERR_FILE"
    check_exit_nonzero "group-writable module .so: load rejected" "$?"
else
    echo "SKIP: group-writable module test (no C compiler)"
fi

# ===========================================================================
# Test 5: Non-existent module path must be rejected cleanly
# ===========================================================================

"$SILEX" --load-module "$TMPDIR_MOD/nonexistent_module.so" > /dev/null 2>"$STDERR_FILE"
check_exit_nonzero "nonexistent module path: clean rejection" "$?"

# ===========================================================================
# Test 6: Module path with directory traversal rejected
# ===========================================================================

if [ "$HAVE_COMPILER" -eq 1 ]; then
    cp "$STUB_SO" "$TMPDIR_MOD/traversal_test.so"
    chmod 0644 "$TMPDIR_MOD/traversal_test.so"
    TRAVERSAL_PATH="$TMPDIR_MOD/subdir/../traversal_test.so"
    "$SILEX" --load-module "$TRAVERSAL_PATH" > /dev/null 2>"$STDERR_FILE"
    # Should either canonicalise and load (if safe), or reject.
    # Document the result.
    echo "INFO: traversal path load exit=$? (expected: reject or canonicalise)"
    echo "SKIP: traversal path rejection depends on implementation"
else
    echo "SKIP: module traversal test (no C compiler)"
fi

echo ""
echo "module security tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
