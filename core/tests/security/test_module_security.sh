#!/bin/sh
# tests/security/test_module_security.sh — the loader's refusal decisions
#
# silex will dlopen a .so into its own address space, so which files it agrees
# to load is a security decision. src/module/loader.c refuses a module that is a
# symlink, is not a regular file, is owned by someone else, is group- or
# world-writable, or sits in a group- or world-writable non-sticky directory.
# This suite proves each refusal actually fires.
#
# WHAT THIS FILE USED TO BE, AND WHY IT MATTERED
#
# Every test here invoked `"$SILEX" --load-module "$SO"` and asserted a non-zero
# exit. silex has no --load-module flag and never had one: it rejected the
# unknown option with "Illegal option --", exit 2, without opening the file --
# or reading the module directory, or entering loader.c at all. The assertions
# were therefore true no matter what the fixtures contained, and would have
# stayed true with src/module/loader.c deleted from the build.
#
# The suite reported "4 passed" for as long as git remembers, while `make
# coverage` reported 0.0% for loader.c. The green result was what stopped anyone
# from reading the 0.0%.
#
# So: two rules for anything added below.
#
#   1. Drive the REAL path. The loader is reached through SILEX_MODULE_PATH when
#      an applet meets a flag it does not know; that is the only way in.
#   2. Every silent refusal gets a CONTROL. Most rejections here are silent --
#      the flag just looks unrecognised, exactly as it would if no module were
#      present. An assertion on that alone proves nothing. Load the very same
#      .so successfully first, then change the one thing under test.
#
# Fixtures live under /tmp, never in the source tree: on WSL2 the tree is on
# drvfs, where chmod is a silent no-op, so the permission cases would set no
# bits and "pass" without exercising anything.

SILEX="${1:-build/bin/silex}"
case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;;
esac
if [ ! -x "$SILEX" ]; then
    echo "ERROR: no silex at '$SILEX'" >&2
    exit 1
fi

PASS=0
FAIL=0

ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; echo "      $2"; FAIL=$((FAIL + 1)); }

# loads <dir>  -- true if the test module in <dir> answers for --silex-sec-flag
loads() {
    SILEX_MODULE_PATH="$1" "$SILEX" -c 'grep --silex-sec-flag x' 2>&1 \
        | grep -q '^MODULE-OK$'
}

# refused <name> <dir>            -- module must NOT load
refused() {
    if loads "$2"; then
        bad "$1" "the module loaded when it should have been refused"
    else
        ok "$1"
    fi
}

# accepted <name> <dir>           -- module MUST load (the control)
accepted() {
    if loads "$2"; then
        ok "$1"
    else
        bad "$1" "the control module did not load: $(SILEX_MODULE_PATH="$2" \
                  "$SILEX" -c 'grep --silex-sec-flag x' 2>&1 | head -2)"
    fi
}

# says <name> <pattern> <dir>     -- module refused WITH this diagnostic
says() {
    _out=$(SILEX_MODULE_PATH="$3" "$SILEX" -c 'grep --silex-sec-flag x' 2>&1)
    case "$_out" in
        *"$2"*) ok "$1" ;;
        *)      bad "$1" "expected a diagnostic containing [$2], got [$_out]" ;;
    esac
}

if ! command -v gcc >/dev/null 2>&1; then
    echo "SKIP: no gcc -- cannot build a module to test the loader with"
    echo ""
    echo "module security tests: 0 passed, 0 failed (skipped)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE=$(TMPDIR=/tmp mktemp -d) || { echo "ERROR: mktemp failed" >&2; exit 1; }
trap 'chmod -R u+rwx "$BASE" 2>/dev/null; rm -rf "$BASE"' EXIT INT TERM

# The module must carry the runtime's own libc tag or it is refused for that
# reason instead of the one under test.
if "$SILEX" --version 2>&1 | grep -qi musl; then
    LIBC_DEF=-DSILEX_LIBC_MUSL=1
else
    LIBC_DEF=
fi

cat > "$BASE/m.c" <<'EOF'
#include "silex_module.h"
#include <stdio.h>
static const char *flags[] = { "--silex-sec-flag", NULL };
static int handler(int argc, char **argv, int flag_index)
{
    (void)argc; (void)argv; (void)flag_index;
    printf("MODULE-OK\n");
    return 0;
}
static silex_module_t mod = {
    SILEX_MODULE_API_VERSION, SILEX_LIBC_NAME, "grep", "sec_test",
    "fixture for the loader's security checks", flags, handler
};
SILEX_EXPORT silex_module_t *silex_module_init(void) { return &mod; }
EOF

if ! gcc -shared -fPIC -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
        $LIBC_DEF -I"$ROOT" -o "$BASE/m.so" "$BASE/m.c" 2>"$BASE/build.log"; then
    echo "FAIL: could not build the fixture module"
    cat "$BASE/build.log"
    exit 1
fi

# A fixture directory the loader is happy with: 0755, ours, holding a 0644 copy.
newdir() {
    mkdir -p "$1" && chmod 0755 "$1" && cp "$BASE/m.so" "$1/m.so" && chmod 0644 "$1/m.so"
}

# --- 0. the baseline --------------------------------------------------------
# If this fails, every refusal below is meaningless -- they would all "pass"
# because nothing loads at all.
newdir "$BASE/baseline"
accepted "module security: (baseline) a well-formed module in a safe dir loads" \
         "$BASE/baseline"

# --- 1. a symlinked .so -----------------------------------------------------
# O_NOFOLLOW in module_load, plus an lstat pre-filter in the directory scan.
mkdir -p "$BASE/link" && chmod 0755 "$BASE/link"
ln -s "$BASE/m.so" "$BASE/link/m.so"
refused "module security: a symlinked .so is refused" "$BASE/link"
cp "$BASE/m.so" "$BASE/link/real.so" && chmod 0644 "$BASE/link/real.so"
accepted "module security: (control) the same bytes load when not a symlink" \
         "$BASE/link"

# --- 2. a group- or world-writable .so --------------------------------------
# Anyone who can write the file can choose the code silex runs.
newdir "$BASE/wworld"
accepted "module security: (control) loads before the world-write bit is set" \
         "$BASE/wworld"
chmod 0666 "$BASE/wworld/m.so"
if [ "$(stat -c '%a' "$BASE/wworld/m.so" 2>/dev/null)" = "666" ]; then
    refused "module security: a world-writable .so is refused" "$BASE/wworld"
else
    echo "SKIP: chmod did not take effect (not a POSIX filesystem?)"
fi

newdir "$BASE/wgroup"
accepted "module security: (control) loads before the group-write bit is set" \
         "$BASE/wgroup"
chmod 0664 "$BASE/wgroup/m.so"
if [ "$(stat -c '%a' "$BASE/wgroup/m.so" 2>/dev/null)" = "664" ]; then
    refused "module security: a group-writable .so is refused" "$BASE/wgroup"
    # Group-writable is caught by module_load, which says so out loud. Only
    # S_IWOTH used to be checked, so a root-owned .so writable by a group the
    # attacker belongs to loaded happily.
    says "module security: ...and names the reason" "group-writable" "$BASE/wgroup"
else
    echo "SKIP: chmod did not take effect (not a POSIX filesystem?)"
fi

# --- 3. a writable module DIRECTORY -----------------------------------------
# Write access to the directory means the file can simply be replaced.
newdir "$BASE/wwdir"
accepted "module security: (control) loads before the dir is opened up" \
         "$BASE/wwdir"
chmod 0777 "$BASE/wwdir"
if [ "$(stat -c '%a' "$BASE/wwdir" 2>/dev/null)" = "777" ]; then
    refused "module security: a world-writable module dir is refused" "$BASE/wwdir"
    says "module security: ...and names the reason" "world-writable" "$BASE/wwdir"
    chmod 0755 "$BASE/wwdir"
else
    echo "SKIP: chmod did not take effect (not a POSIX filesystem?)"
fi

# A sticky directory is the documented exception: /tmp is world-writable, but
# only an entry's owner may remove it, so the swap the check guards against
# cannot happen.
newdir "$BASE/sticky"
chmod 1777 "$BASE/sticky"
if [ "$(stat -c '%a' "$BASE/sticky" 2>/dev/null)" = "1777" ]; then
    accepted "module security: a STICKY world-writable dir is still allowed" \
             "$BASE/sticky"
    chmod 0755 "$BASE/sticky"
else
    echo "SKIP: chmod did not take effect (not a POSIX filesystem?)"
fi

# --- 4. things that are not modules -----------------------------------------
mkdir -p "$BASE/notfile" && chmod 0755 "$BASE/notfile"
mkdir -p "$BASE/notfile/m.so"
refused "module security: a directory named *.so is not loaded" "$BASE/notfile"
says "module security: ...and names the reason" "not a regular file" "$BASE/notfile"

mkdir -p "$BASE/noinit" && chmod 0755 "$BASE/noinit"
echo 'int unrelated(void) { return 0; }' > "$BASE/noinit/n.c"
gcc -shared -fPIC -O0 -o "$BASE/noinit/n.so" "$BASE/noinit/n.c" 2>/dev/null
chmod 0644 "$BASE/noinit/n.so"
refused "module security: a .so exporting no silex_module_init is refused" \
        "$BASE/noinit"
says "module security: ...and names the reason" "silex_module_init" "$BASE/noinit"

# A missing module directory must be a quiet miss, not an error or a crash.
refused "module security: a nonexistent module dir is a clean miss" \
        "$BASE/does-not-exist"

# A path-traversal spelling of a legitimate directory is neither privileged nor
# rejected: the checks are anchored to the opened descriptor, so how the path
# was spelled does not matter. It must behave exactly like the direct spelling.
newdir "$BASE/sub"
accepted "module security: a ../ spelling of a safe dir behaves the same" \
         "$BASE/sub/../sub"

echo ""
echo "module security tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
