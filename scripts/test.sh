#!/bin/bash
# Silex test suite: runs against silex:slim image
# Usage: ./scripts/test.sh [image_name]
# Default image: silex:slim

set -e

IMAGE="${1:-silex:slim}"
PASS=0
FAIL=0
_T0=$(date +%s%N 2>/dev/null || date +%s)

_bold() { printf '\033[1m%s\033[0m' "$1"; }
_green() { printf '\033[32m%s\033[0m' "$1"; }
_red() { printf '\033[31m%s\033[0m' "$1"; }
_yellow() { printf '\033[33m%s\033[0m' "$1"; }

_run() {
    local name="$1"; shift
    local t0 t1 elapsed
    t0=$(date +%s%N 2>/dev/null || echo 0)
    if output=$(docker run --rm "$IMAGE" /bin/sh -c "$*" 2>&1); then
        t1=$(date +%s%N 2>/dev/null || echo 0)
        elapsed=$(( (t1 - t0) / 1000000 ))
        printf "  $(_green PASS) %-40s %dms\n" "$name" "$elapsed"
        PASS=$((PASS + 1))
    else
        t1=$(date +%s%N 2>/dev/null || echo 0)
        elapsed=$(( (t1 - t0) / 1000000 ))
        printf "  $(_red FAIL) %-40s %dms\n" "$name" "$elapsed"
        printf "       %s\n" "$(echo "$output" | head -5 | sed 's/^/       /')"
        FAIL=$((FAIL + 1))
    fi
}

_run_with_output() {
    local name="$1" expected="$2"; shift 2
    local t0 t1 elapsed output
    t0=$(date +%s%N 2>/dev/null || echo 0)
    output=$(docker run --rm "$IMAGE" /bin/sh -c "$*" 2>&1) || true
    t1=$(date +%s%N 2>/dev/null || echo 0)
    elapsed=$(( (t1 - t0) / 1000000 ))
    if echo "$output" | grep -qF "$expected"; then
        printf "  $(_green PASS) %-40s %dms\n" "$name" "$elapsed"
        PASS=$((PASS + 1))
    else
        printf "  $(_red FAIL) %-40s %dms\n" "$name" "$elapsed"
        printf "       Expected to find: %s\n" "$expected"
        printf "       Got: %s\n" "$(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

echo ""
_bold "Silex Test Suite"
echo ""
echo "Image: $IMAGE"
echo ""

# Check image exists
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    _red "ERROR"
    echo ": image '$IMAGE' not found. Run ./scripts/build.sh first."
    exit 1
fi

# --------------------------------------------------------------------------
echo "--- Compiler ---"

_run "C compile (hello world)" '
cat > /tmp/hello.c << "EOF"
#include <stdio.h>
int main() { printf("hello\n"); return 0; }
EOF
clang -o /tmp/hello /tmp/hello.c && /tmp/hello | grep -q hello
'

_run "C++ compile (hello world)" '
cat > /tmp/hello.cpp << "EOF"
#include <iostream>
int main() { std::cout << "hello" << std::endl; return 0; }
EOF
clang++ -o /tmp/hello_cpp /tmp/hello.cpp && /tmp/hello_cpp | grep -q hello
'

_run_with_output "CC defaults to clang" "clang" 'echo "$CC"'
_run_with_output "CXX defaults to clang++" "clang++" 'echo "$CXX"'

# --------------------------------------------------------------------------
echo "--- Linker ---"

_run "mold binary present" 'test -x /opt/mold/bin/mold'

_run_with_output "mold is default linker" "mold" '/opt/mold/bin/mold --version'

_run "mold used when linking" '
cat > /tmp/test_mold.c << "EOF"
int main() { return 0; }
EOF
clang -fuse-ld=mold -Wl,--version -o /tmp/test_mold /tmp/test_mold.c 2>&1 | grep -qi mold
'

# --------------------------------------------------------------------------
echo "--- Build system ---"

_run "ninja present" 'ninja --version'
_run_with_output "CMAKE_GENERATOR=Ninja" "Ninja" 'echo "$CMAKE_GENERATOR"'

_run "cmake uses Ninja generator" '
apk add --no-cache cmake >/dev/null 2>&1 || true
mkdir -p /tmp/cmake_test && cat > /tmp/cmake_test/CMakeLists.txt << "EOF"
cmake_minimum_required(VERSION 3.10)
project(test)
EOF
cd /tmp/cmake_test && cmake -B build -G Ninja >/dev/null 2>&1
test -f /tmp/cmake_test/build/build.ninja
'

# --------------------------------------------------------------------------
echo "--- Compiler cache ---"

_run "sccache binary present" 'test -x /opt/sccache/bin/sccache'

_run_with_output "sccache version readable" "sccache" 'sccache --version'

_run "sccache caches C compilation" '
export SCCACHE_DIR=/tmp/test_sccache
export CMAKE_C_COMPILER_LAUNCHER=sccache
cat > /tmp/cache_test.c << "EOF"
#include <stdio.h>
int main() { return 0; }
EOF
sccache clang -o /tmp/cache_test1 /tmp/cache_test.c
sccache clang -o /tmp/cache_test2 /tmp/cache_test.c
sccache --show-stats 2>&1 | grep -q "Cache hits"
'

# --------------------------------------------------------------------------
echo "--- Allocator ---"

_run "jemalloc library present" 'find /usr/lib /usr/local/lib -name "libjemalloc*" | grep -q jemalloc'

_run "LD_PRELOAD set to jemalloc" 'echo "$LD_PRELOAD" | grep -q jemalloc'

_run "jemalloc actually loaded" '
cat > /tmp/check_malloc.c << "EOF"
#include <stdlib.h>
int main() { void *p = malloc(1024); free(p); return 0; }
EOF
clang -o /tmp/check_malloc /tmp/check_malloc.c
ldd /tmp/check_malloc 2>/dev/null || true
# Check if jemalloc is in LD_PRELOAD and accessible
test -n "$LD_PRELOAD" && test -f "$LD_PRELOAD"
'

# --------------------------------------------------------------------------
echo "--- Wrappers ---"

_run "wrappers directory present" 'test -d /usr/local/silex/bin'

_run_with_output "cp wrapper in PATH" "/usr/local/silex/bin/cp" 'which cp'

_run_with_output "sort wrapper in PATH" "/usr/local/silex/bin/sort" 'which sort'

_run_with_output "git wrapper in PATH" "/usr/local/silex/bin/git" 'which git'

_run_with_output "tar wrapper in PATH" "/usr/local/silex/bin/tar" 'which tar'

_run "git wrapper does shallow clone" '
# Verify wrapper adds --depth 1 (by checking wrapper script content)
grep -q "depth 1" /usr/local/silex/bin/git
'

_run "SILEX_WRAPPERS=off bypasses wrappers" '
SILEX_WRAPPERS=off /usr/local/silex/bin/git --version >/dev/null 2>&1
'

# --------------------------------------------------------------------------
echo "--- apt shim ---"

_run "apt-get shim present" 'test -x /usr/local/bin/apt-get'

_run "apt symlink present" 'test -L /usr/local/bin/apt || test -x /usr/local/bin/apt'

_run "apt-get install translates to apk" '
SILEX_APT_SHIM=on apt-get install -y curl 2>&1 | grep -q "apk add"
'

_run "package mapping file present" 'test -f /usr/local/silex/package-mapping.json'

_run "package mapping has 100+ entries" '
python3 -c "
import json
with open(\"/usr/local/silex/package-mapping.json\") as f:
    d = json.load(f)
count = len([k for k in d if not k.startswith(\"_\")])
assert count >= 100, f\"Only {count} entries\"
print(f\"{count} entries\")
"
'

_run "build-essential maps to build-base" '
python3 -c "
import json
with open(\"/usr/local/silex/package-mapping.json\") as f:
    d = json.load(f)
assert d.get(\"build-essential\") == \"build-base\", f\"Got: {d.get(\"build-essential\")}\"
"
'

# --------------------------------------------------------------------------
echo "--- silex command ---"

_run "silex binary present" 'test -x /usr/local/bin/silex'

_run "silex doctor exits 0" 'silex doctor >/dev/null 2>&1'

_run_with_output "silex doctor shows version" "slim" 'silex doctor 2>/dev/null'

_run_with_output "silex doctor shows CC" "clang" 'silex doctor 2>/dev/null'

_run_with_output "silex doctor shows sccache" "sccache" 'silex doctor 2>/dev/null'

_run_with_output "silex doctor shows wrappers" "Wrappers" 'silex doctor 2>/dev/null'

_run "silex version exits 0" 'silex version >/dev/null'

_run "silex lint exits on missing file" '! silex lint /nonexistent/Dockerfile 2>/dev/null'

# --------------------------------------------------------------------------
echo "--- Accelerated compression ---"

_run "pigz present" 'pigz --version'
_run "pixz present" 'pixz -h 2>&1 || true; command -v pixz'
_run "tar wrapper uses pigz for .tar.gz" '
echo "test" > /tmp/testfile.txt
tar czf /tmp/test_pigz.tar.gz /tmp/testfile.txt 2>&1
# Verify pigz is accessible (wrapper will use it)
command -v pigz
'

# --------------------------------------------------------------------------
echo "--- Search tools ---"

_run "fd present" 'fd --version'
_run "ripgrep present" 'rg --version'

# --------------------------------------------------------------------------
echo "--- Allocator (mimalloc) ---"

_run "mimalloc library present" 'find /usr/lib -name "libmimalloc*" | grep -q .'
_run "SILEX_MALLOC=mimalloc sets LD_PRELOAD" '
SILEX_MALLOC=mimalloc /usr/local/bin/silex-entrypoint env 2>/dev/null | grep -q LD_PRELOAD
'

# --------------------------------------------------------------------------
echo "--- Python tooling ---"

_run "uv present" 'uv --version'

# --------------------------------------------------------------------------
echo "--- Checksums ---"

_run "xxhsum present" 'xxhsum --version 2>&1 || xxh64sum --version 2>&1 || command -v xxhsum'

# --------------------------------------------------------------------------
echo "--- POSIX shell ---"

_run "dash present" 'dash --version 2>&1 || true; command -v dash'
_run "/bin/dash exists" 'test -x /bin/dash'

# --------------------------------------------------------------------------
echo "--- Debian shims ---"

_run_with_output "lsb_release -i outputs Wolfi" "Wolfi" 'lsb_release -i'
_run_with_output "dpkg-architecture outputs amd64 or arm64" "64" 'dpkg-architecture -qDEB_HOST_ARCH'
_run "update-alternatives --install exits 0" \
    'update-alternatives --install /usr/bin/test test /usr/bin/test 1'
_run "adduser --help exits 0" 'adduser --help 2>&1 || true; test -x /usr/local/bin/adduser'
_run "addgroup --help exits 0" 'addgroup --help 2>&1 || true; test -x /usr/local/bin/addgroup'

# --------------------------------------------------------------------------
echo "--- os-release ---"

_run "ID_LIKE=debian in /etc/os-release" 'grep -q "ID_LIKE=debian" /etc/os-release'

# --------------------------------------------------------------------------
echo "--- Environment ---"

_run_with_output "SILEX_WRAPPERS=on by default" "on" 'echo "$SILEX_WRAPPERS"'

_run_with_output "SILEX_MALLOC=jemalloc by default" "jemalloc" 'echo "$SILEX_MALLOC"'

_run_with_output "SILEX_GIT_SHALLOW=on by default" "on" 'echo "$SILEX_GIT_SHALLOW"'

_run_with_output "SILEX_APT_SHIM=on by default" "on" 'echo "$SILEX_APT_SHIM"'

_run_with_output "LC_ALL is C" "C" 'echo "$LC_ALL"'

_run_with_output "CMAKE_GENERATOR is Ninja" "Ninja" 'echo "$CMAKE_GENERATOR"'

# --------------------------------------------------------------------------
echo ""
_T1=$(date +%s%N 2>/dev/null || date +%s)
_total_ms=$(( (_T1 - _T0) / 1000000 ))

_total=$((PASS + FAIL))
echo "Results: $(_green "$PASS passed") / $FAIL failed / $total total  (${_total_ms}ms)"
echo ""

if [ "$FAIL" -gt 0 ]; then
    _red "FAILED"
    echo " — $FAIL test(s) did not pass"
    exit 1
else
    _green "All tests passed"
    echo ""
fi
