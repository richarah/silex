#!/bin/bash
# test_module.sh — the dynamic module loader and registry
#
# src/module/loader.c and src/module/registry.c are ~600 lines that ship in
# every silex binary and had no test of any kind: `make coverage` reported 0.0%
# for both. That is the wrong shape of code to leave uncovered -- loader.c is
# security-relevant (it decides which .so gets dlopen'd into the shell) and its
# checks are exactly the kind that rot silently, because a check that stops
# rejecting looks identical from the outside to one that never fired.
#
# Everything here is driven through the real binary via SILEX_MODULE_PATH, so it
# tests the shipped path rather than a stand-in.
#
# WHY /tmp AND NOT THE PROJECT TREE: the loader rejects group- and
# world-writable modules, and several cases below turn those bits ON to prove
# the rejection fires. On WSL2 the project lives on drvfs, where chmod is a
# silent no-op -- the bits never change and those cases would "pass" without
# ever exercising the check. Every fixture is built under /tmp, on a real
# filesystem. (Same trap that made 27 coreutils tests look broken; see
# tests/external/run-gnu-coreutils-native.sh.)

set -u

SILEX="${1:-build/bin/silex}"
case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;;
esac
[ -x "$SILEX" ] || { echo "ERROR: no silex at '$SILEX'" >&2; exit 1; }

PASS=0
FAIL=0

ok()   { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "FAIL: $1"; echo "      $2"; FAIL=$((FAIL + 1)); }

# check <name> <expected-substring> <actual>
check() {
    case "$3" in
        *"$2"*) ok "$1" ;;
        *)      bad "$1" "expected to contain [$2], got [$3]" ;;
    esac
}

# A module is only meaningful if we can build one. Say so out loud rather than
# reporting a green run that tested nothing.
if ! command -v gcc >/dev/null 2>&1; then
    echo "SKIP: no gcc -- cannot build a test module"
    echo ""
    echo "module tests: 0 passed, 0 failed (skipped)"
    exit 0
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMPDIR_BASE=$(TMPDIR=/tmp mktemp -d) || { echo "ERROR: mktemp failed" >&2; exit 1; }
trap 'chmod -R u+w "$TMPDIR_BASE" 2>/dev/null; rm -rf "$TMPDIR_BASE"' EXIT

# Modules must be built against the same libc tag the runtime expects. Read it
# off the binary rather than assuming: a musl build rejects a "glibc" module by
# design, and hardcoding either one would make this suite fail on the other.
if "$SILEX" --version 2>&1 | grep -qi musl; then
    HOST_LIBC=musl
    LIBC_DEF=-DSILEX_LIBC_MUSL=1
else
    HOST_LIBC=glibc
    LIBC_DEF=
fi

# build_module <out.so> <tool> <api_version> <libc-string> <exit-code> <flag...>
#
# Writes and compiles a module whose handler prints a marker naming the flag it
# was dispatched on, then exits with <exit-code>.
build_module() {
    local out="$1" tool="$2" api="$3" libc="$4" rc="$5"; shift 5
    local src="${out%.so}.c" flags="" f
    for f in "$@"; do flags="$flags \"$f\","; done

    cat > "$src" <<EOF
#include "silex_module.h"
#include <stdio.h>

static const char *flags[] = {$flags NULL};

static int handler(int argc, char **argv, int flag_index)
{
    (void)argc;
    printf("MODULE:%s:%s\n", "$tool", argv[flag_index]);
    fflush(stdout);
    return $rc;
}

static silex_module_t mod = {
    $api, "$libc", "$tool", "test_module",
    "throwaway module for tests", flags, handler
};

SILEX_EXPORT silex_module_t *silex_module_init(void) { return &mod; }
EOF
    gcc -shared -fPIC -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
        $LIBC_DEF -I"$ROOT" -o "$out" "$src" 2>"${out}.buildlog"
}

API=$(sed -n 's/^#define SILEX_MODULE_API_VERSION \([0-9]*\).*/\1/p' "$ROOT/silex_module.h")
[ -n "$API" ] || { echo "ERROR: cannot read SILEX_MODULE_API_VERSION" >&2; exit 1; }

# --- 1. the happy path ------------------------------------------------------
D1="$TMPDIR_BASE/ok"; mkdir -p "$D1"
if ! build_module "$D1/good.so" grep "$API" "$HOST_LIBC" 0 --silex-test-flag; then
    echo "FAIL: could not build the test module"
    cat "$D1/good.so.buildlog" 2>/dev/null
    exit 1
fi

out=$(SILEX_MODULE_PATH="$D1" "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: an unknown grep flag dispatches to the module" \
      "MODULE:grep:--silex-test-flag" "$out"

# The handler's return value is the applet's exit status, not a fixed 0 --
# a module that fails must be able to say so.
D2="$TMPDIR_BASE/rc"; mkdir -p "$D2"
build_module "$D2/rc.so" grep "$API" "$HOST_LIBC" 3 --silex-rc-flag
SILEX_MODULE_PATH="$D2" "$SILEX" -c 'grep --silex-rc-flag x' >/dev/null 2>&1
rc=$?
[ "$rc" -eq 3 ] && ok "module: the handler's return value becomes the exit status" \
                || bad "module: the handler's return value becomes the exit status" \
                       "expected 3, got $rc"

# A module may advertise several flags; registry_register() records every one,
# so the second must resolve from the cache without another directory scan.
D3="$TMPDIR_BASE/multi"; mkdir -p "$D3"
build_module "$D3/multi.so" grep "$API" "$HOST_LIBC" 0 --silex-alpha --silex-beta
out=$(SILEX_MODULE_PATH="$D3" "$SILEX" -c 'grep --silex-alpha x; grep --silex-beta x' 2>&1)
check "module: every advertised flag is registered (1/2)" "MODULE:grep:--silex-alpha" "$out"
check "module: every advertised flag is registered (2/2)" "MODULE:grep:--silex-beta"  "$out"

# Not a grep-only hook: sort, cp, install and xargs share the mechanism, and
# each must be exercised -- cp and install both HAD a long-option hook missing,
# which made every long flag fall into the short-flag loop and get looked up
# under the name "--". modules/cp_reflink.c advertises nothing but long flags,
# so the module shipped in this repo was unreachable.
D4="$TMPDIR_BASE/sort"; mkdir -p "$D4"
build_module "$D4/s.so" sort "$API" "$HOST_LIBC" 0 --silex-sort-flag
out=$(SILEX_MODULE_PATH="$D4" "$SILEX" -c 'sort --silex-sort-flag' 2>&1)
check "module: the sort applet dispatches too" "MODULE:sort:--silex-sort-flag" "$out"

D4b="$TMPDIR_BASE/xargs"; mkdir -p "$D4b"
build_module "$D4b/x.so" xargs "$API" "$HOST_LIBC" 0 --silex-xargs-flag
out=$(SILEX_MODULE_PATH="$D4b" "$SILEX" -c 'xargs --silex-xargs-flag </dev/null' 2>&1)
check "module: the xargs applet dispatches too" "MODULE:xargs:--silex-xargs-flag" "$out"

D4c="$TMPDIR_BASE/install"; mkdir -p "$D4c"
build_module "$D4c/i.so" install "$API" "$HOST_LIBC" 0 --silex-install-flag
out=$(SILEX_MODULE_PATH="$D4c" "$SILEX" -c 'install --silex-install-flag' 2>&1)
check "module: the install applet dispatches too" "MODULE:install:--silex-install-flag" "$out"


# Several directories, module in the last one: registry_lookup() must walk the
# whole colon-separated SILEX_MODULE_PATH, not just its head.
out=$(SILEX_MODULE_PATH="$TMPDIR_BASE/nonexistent:$D1" \
      "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: SILEX_MODULE_PATH is searched in full" \
      "MODULE:grep:--silex-test-flag" "$out"

# SEVERAL modules in ONE directory -- the arrangement `make install-modules`
# produces, and the one that was broken.
#
# module_load() dlopens "/proc/self/fd/N" and used to close N immediately, so
# every module got the same lowest-free descriptor and therefore the same path
# string. dlopen caches by that string: the second module in a directory came
# back as the FIRST one, silently. At most one module worked per process and
# readdir order picked it -- with the three .so files this repo ships installed
# together, two of them were dead.
#
# Nothing in a single-module test can see this, which is why it survived. Three
# modules, three flags, all three must answer.
D14="$TMPDIR_BASE/many"; mkdir -p "$D14"
build_module "$D14/m1.so" grep "$API" "$HOST_LIBC" 0 --silex-one
build_module "$D14/m2.so" grep "$API" "$HOST_LIBC" 0 --silex-two
build_module "$D14/m3.so" grep "$API" "$HOST_LIBC" 0 --silex-three
for n in one two three; do
    out=$(SILEX_MODULE_PATH="$D14" "$SILEX" -c "grep --silex-$n x" 2>&1)
    check "module: one of three .so files in a directory (--silex-$n)" \
          "MODULE:grep:--silex-$n" "$out"
done

# ...and all three within a SINGLE shell process, which is where the shared
# descriptor number actually bit: the second lookup is the one that used to come
# back as the first module.
out=$(SILEX_MODULE_PATH="$D14" "$SILEX" -c \
      'grep --silex-one x; grep --silex-two x; grep --silex-three x' 2>&1)
n=$(printf '%s\n' "$out" | grep -c '^MODULE:grep:--silex-')
[ "$n" -eq 3 ] && ok "module: three modules load in one process" \
               || bad "module: three modules load in one process" \
                      "expected 3 dispatches, got $n -- output: $out"

# --- 2. modules that must NOT be loaded -------------------------------------

# A module for a different tool must not answer for this one. Proven both ways:
# the same .so DOES answer for the tool it names, so the refusal below is the
# tool check firing and not a module that failed to build.
D5="$TMPDIR_BASE/wrongtool"; mkdir -p "$D5"
build_module "$D5/w.so" cp "$API" "$HOST_LIBC" 0 --silex-test-flag
out=$(SILEX_MODULE_PATH="$D5" "$SILEX" -c 'cp --silex-test-flag' 2>&1)
check "module: (control) the cp module answers for cp" "MODULE:cp:--silex-test-flag" "$out"

# flag_index must be the argv slot the flag was actually found in, not a
# hardcoded 1 -- modules index argv with it (cp_reflink reads argv[flag_index]
# to choose its mode), so a flag anywhere but first made them read the wrong
# argument. The marker prints argv[flag_index], so a wrong index shows up here.
out=$(SILEX_MODULE_PATH="$D5" "$SILEX" -c 'cp -v --silex-test-flag a b' 2>&1)
check "module: flag_index points at the flag, not at argv[1]" \
      "MODULE:cp:--silex-test-flag" "$out"

out=$(SILEX_MODULE_PATH="$D5" "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: a module for another tool does not answer" "unrecognized option" "$out"

# An ABI break must be refused, not called. This is the check that matters most
# on upgrade: a stale .so whose struct layout has moved is a wild call.
D6="$TMPDIR_BASE/api"; mkdir -p "$D6"
build_module "$D6/a.so" grep "$((API + 1))" "$HOST_LIBC" 0 --silex-test-flag
out=$(SILEX_MODULE_PATH="$D6" "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: an API version mismatch is refused" "API version mismatch" "$out"

# Wrong libc tag: a musl module inside a glibc shell (or the reverse).
D7="$TMPDIR_BASE/libc"; mkdir -p "$D7"
if [ "$HOST_LIBC" = glibc ]; then WRONG_LIBC=musl; else WRONG_LIBC=glibc; fi
build_module "$D7/l.so" grep "$API" "$WRONG_LIBC" 0 --silex-test-flag
out=$(SILEX_MODULE_PATH="$D7" "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: a libc tag mismatch is refused" "libc mismatch" "$out"

# A .so with no silex_module_init: dlsym must fail cleanly, not crash.
D8="$TMPDIR_BASE/noinit"; mkdir -p "$D8"
cat > "$D8/n.c" <<'EOF'
int unrelated(void) { return 0; }
EOF
gcc -shared -fPIC -O0 -o "$D8/n.so" "$D8/n.c" 2>/dev/null
out=$(SILEX_MODULE_PATH="$D8" "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: a .so without silex_module_init is refused" "silex_module_init" "$out"

# --- 3. the security checks -------------------------------------------------
#
# Deliberately NOT here. The loader refuses symlinks, group- and
# world-writable modules, writable module directories, and non-regular files;
# each of those lives in tests/security/test_module_security.sh, with the
# control that proves the refusal is what stopped the load rather than a
# fixture that never worked. Keeping them there means `make security-test`
# covers the security decisions and this file covers dispatch and the registry,
# with neither duplicating the other.

# --- 4. the registry --------------------------------------------------------

# A miss is cached as a NEGATIVE entry, so the second lookup of the same flag
# costs a hash probe rather than a rescan-and-dlopen of every .so present. Both
# lookups must still report the miss.
out=$(SILEX_MODULE_PATH="$D1" \
      "$SILEX" -c 'grep --silex-absent x; grep --silex-absent x' 2>&1)
n=$(printf '%s\n' "$out" | grep -c "unrecognized option")
[ "$n" -eq 2 ] && ok "module: a cached miss still reports the miss" \
               || bad "module: a cached miss still reports the miss" \
                      "expected 2 diagnostics, got $n"

# A hit is cached too, and the cached module must behave like the fresh one.
out=$(SILEX_MODULE_PATH="$D1" \
      "$SILEX" -c 'grep --silex-test-flag x; grep --silex-test-flag y' 2>&1)
n=$(printf '%s\n' "$out" | grep -c "MODULE:grep:--silex-test-flag")
[ "$n" -eq 2 ] && ok "module: a cached hit dispatches again" \
               || bad "module: a cached hit dispatches again" \
                      "expected 2 dispatches, got $n"

# No module path at all: the default directory almost certainly does not exist
# here, and that is not an error -- an unknown flag is simply unknown.
out=$(env -u SILEX_MODULE_PATH "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: an absent module directory is not an error" "unrecognized option" "$out"

# An empty SILEX_MODULE_PATH must fall back to the default, not scan "".
out=$(SILEX_MODULE_PATH= "$SILEX" -c 'grep --silex-test-flag x' 2>&1)
check "module: an empty SILEX_MODULE_PATH falls back" "unrecognized option" "$out"

echo ""
echo "module tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
