#!/bin/sh
# bench_shell.sh — silex vs dash on a real build, with the artifact asserted.
#
# THE RULE THIS FILE EXISTS TO ENFORCE:
#
#   A failed run is not a fast run.
#
# This project has been burned by that twice. sdk/benchmarks/benchmark.sh used to
# end every timed command in `|| true`, with a comment cheerfully noting that
# "failed runs produce a measurement of time-to-fail" -- so a silex run that died
# because a package was missing was averaged in as a 3x win. And when we measured
# silex-as-/bin/sh against dash, silex looked 44% faster on `configure + make`;
# it was actually FAILING, building 16 of 19 objects and never producing libz.a.
#
# So: every run here asserts the artifact. Not the exit code -- the ARTIFACT.
# Count the object files. Stat the library. If you cannot say what the build was
# supposed to produce, you cannot benchmark it.
#
# Usage: bench_shell.sh [path/to/silex] [path/to/zlib-source]

set -u

SILEX="${1:-build/bin/silex}"
SRC="${2:-tests/external/repos/projects/zlib}"
REPS="${BENCH_REPS:-3}"

case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;;
esac
case "$SRC" in
    /*) ;;
    *)  SRC="$(cd "$SRC" 2>/dev/null && pwd)" ;;
esac

[ -x "$SILEX" ]        || { echo "bench: no silex at $SILEX" >&2; exit 1; }
[ -f "$SRC/configure" ] || { echo "bench: no zlib source at $SRC" >&2; exit 1; }

REF=$(command -v dash || command -v sh)
echo "=== shell benchmark ==="
echo "silex:     $SILEX"
echo "reference: $REF"
echo "source:    $SRC"
echo ""

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

# EXPECTED ARTIFACTS. If a run does not produce these, it did not build zlib,
# and its time is meaningless.
EXPECT_OBJS=19
EXPECT_LIB=libz.a

# run_once <shell> <do_make>  -> prints elapsed ms, or FAILED
run_once() {
    _sh="$1"
    _make="$2"

    rm -rf "$WORK/z"
    cp -r "$SRC" "$WORK/z" 2>/dev/null || return 1
    cd "$WORK/z" || return 1
    make distclean >/dev/null 2>&1

    _t0=$(date +%s%N)
    CONFIG_SHELL="$_sh" "$_sh" ./configure >/dev/null 2>&1 || { echo FAILED; return 0; }
    if [ "$_make" = "make" ]; then
        make -j4 SHELL="$_sh" >/dev/null 2>&1 || { echo FAILED; return 0; }
    fi
    _t1=$(date +%s%N)

    # ---- assert the artifact ----
    if [ "$_make" = "make" ]; then
        _objs=$(ls ./*.o 2>/dev/null | wc -l)
        if [ ! -f "$EXPECT_LIB" ]; then
            echo "FAILED"; return 0
        fi
        if [ "$_objs" -ne "$EXPECT_OBJS" ]; then
            echo "FAILED"; return 0
        fi
    else
        # configure must have produced a Makefile that can actually build
        [ -f Makefile ] || { echo FAILED; return 0; }
    fi

    echo $(( (_t1 - _t0) / 1000000 ))
}

# best_of <shell> <do_make> -> best ms, or FAILED if any rep failed
best_of() {
    _best=""
    _i=0
    while [ "$_i" -lt "$REPS" ]; do
        _ms=$(run_once "$1" "$2")
        if [ "$_ms" = "FAILED" ]; then
            echo "FAILED"
            return 0
        fi
        if [ -z "$_best" ] || [ "$_ms" -lt "$_best" ]; then
            _best=$_ms
        fi
        _i=$((_i + 1))
    done
    echo "$_best"
}

STATUS=0
printf '%-14s %14s %18s\n' "/bin/sh" "configure" "configure+make"
printf '%s\n' "----------------------------------------------------"

for _sh in "$REF" "$SILEX"; do
    _c=$(best_of "$_sh" "")
    _m=$(best_of "$_sh" "make")
    printf '%-14s %11s ms %15s ms\n' "$(basename "$_sh")" "$_c" "$_m"
    if [ "$_c" = "FAILED" ] || [ "$_m" = "FAILED" ]; then
        STATUS=1
    fi
    # remember silex's numbers for the summary
    if [ "$_sh" = "$SILEX" ]; then SC=$_c; SM=$_m; else RC=$_c; RM=$_m; fi
done

echo ""
if [ "$STATUS" -ne 0 ]; then
    echo "BENCHMARK FAILED: a shell did not produce $EXPECT_LIB with $EXPECT_OBJS objects."
    echo "A failed run is not a fast run. Refusing to report a time."
    exit 1
fi

# Report, and gate on not having REGRESSED badly against the reference. We do not
# gate on being faster -- that is a product decision, not a build error -- but a
# large regression means something broke.
echo "silex vs $(basename "$REF"):"
awk -v sc="$SC" -v rc="$RC" -v sm="$SM" -v rm="$RM" 'BEGIN {
    printf "  configure:      %+.1f%%\n", (rc - sc) * 100.0 / rc;
    printf "  configure+make: %+.1f%%\n", (rm - sm) * 100.0 / rm;
    printf "  (positive = silex faster)\n";
}'

# Regression gate: silex must not be more than 50% SLOWER than the reference on
# a full build. Deliberately loose -- this catches a catastrophe (a pathological
# glob, an arena that re-grows every cycle), not normal noise.
LIMIT=$(( RM * 3 / 2 ))
if [ "$SM" -gt "$LIMIT" ]; then
    echo ""
    echo "REGRESSION: silex configure+make ${SM}ms is more than 50% slower than ${RM}ms."
    exit 1
fi

echo ""
echo "OK: artifacts verified, no catastrophic regression."
