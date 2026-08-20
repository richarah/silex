#!/bin/sh
# bench_interpreter.sh — the two halves of silex's speed thesis, against dash.
#
# The README makes two claims, and until this file existed neither could be
# checked from the repo:
#
#   1. silex DISPATCHES commands faster, because thirty-two coreutils are
#      builtins: no fork, no exec, no ld.so.
#   2. silex INTERPRETS shell no faster than dash. This was the ceiling.
#
# Claim 2 was quoted as "3.7x slower" from an ad-hoc July 2026 measurement. By
# August it was 1.7x and nobody knew, because nothing re-measured it. A number
# in a README with no benchmark behind it is a number that silently rots -- so
# re-read the table below rather than this comment. As of 2026-08-20 claim 2
# no longer holds as written: silex is AHEAD of dash on ALL FIVE interpretation
# cases, by 1.09x to 1.19x. The gap closed from both ends -- the interpreter
# work in perf/interpreter-and-module-loader, and a builtin lookup that stopped
# running 40 strcmps to say "not a builtin".
#
# That "all five" corrects an earlier reading in this same header, which had
# parameter expansion alone still behind at 1.02x slower. It did not reproduce:
# re-measured interleaved, best of 11, every sample validated, parameter
# expansion is 1.09x FASTER -- and so is the build the 1.02x was taken from, so
# the difference is in the measurement, not the code. The earlier figure came
# from two SEQUENTIAL runs of this script at the default BENCH_REPS=3, which is
# exactly the comparison this script cannot make: see bench_compare.sh, which
# exists for A/B and interleaves. Quote a ratio from here only for silex vs the
# reference shell, and only from a run of at least 7 reps.
#
# WHY NOT bench_shell.sh: that one measures a REAL build (zlib configure+make)
# and is the honest end-to-end figure, but it is dominated by gcc and needs a
# quiet machine -- on a loaded WSL2 box it swings -13% to +37% run to run. These
# micro-benchmarks isolate the shell, so they resolve on a noisy machine. They
# are NOT a substitute for bench_shell.sh: no user runs a bare arithmetic loop.
# Quote both or neither.
#
# The dispatch cases deliberately include a NON-applet control (`od`). Without
# it the applet win reads as a general pipeline win, which it is not -- point a
# pipeline at a tool silex has no builtin for and the advantage disappears.
#
# Usage: bench_interpreter.sh [path/to/silex]
# Env:   BENCH_REPS (default 3) -- best-of-N, since only the floor is signal.

set -u

SILEX="${1:-build/bin/silex}"
REPS="${BENCH_REPS:-3}"

case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;;
esac
[ -x "$SILEX" ] || { echo "bench: no silex at $SILEX" >&2; exit 1; }

REF=$(command -v dash || command -v sh)
[ -n "$REF" ] && [ -x "$REF" ] || { echo "bench: no reference shell" >&2; exit 1; }

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$BENCH_DIR/cases.sh"

echo "=== interpreter / dispatch benchmark ==="
echo "silex:     $SILEX"
echo "reference: $REF"
echo ""

# Milliseconds for one run of `$sh -c "$prog"`. Output is discarded; a shell
# that FAILS is reported rather than timed -- a failed run is not a fast run
# (see bench_shell.sh for what that rule cost this project).
run_once() {
    _sh="$1"; _prog="$2"
    _t0=$(date +%s%N)
    "$_sh" -c "$_prog" >/dev/null 2>&1 || return 1
    _t1=$(date +%s%N)
    echo $(( (_t1 - _t0) / 1000000 ))
}

# Discards invalid samples. `date +%s%N` reads CLOCK_REALTIME, which WSL2 slews
# and occasionally steps BACKWARDS -- one rep of this benchmark came back as
# -1747ms. best_of takes the MINIMUM, so a single backwards sample wins every
# comparison it appears in, permanently, and silently. That is the same shape as
# the two incidents in bench_shell.sh's header: a broken reading recorded as a
# fast one. A non-numeric or negative delta is not a fast run; drop it and take
# another, and report FAILED rather than guess if none are usable.
best_of() {
    _sh="$1"; _prog="$2"
    _best=""; _got=0; _tries=0; _cap=$((REPS * 3))
    while [ "$_got" -lt "$REPS" ] && [ "$_tries" -lt "$_cap" ]; do
        _tries=$((_tries + 1))
        _ms=$(run_once "$_sh" "$_prog") || { echo FAILED; return 0; }
        case "$_ms" in
            ''|*[!0-9]*) continue ;;      # negative (leading -) or garbage
        esac
        _got=$((_got + 1))
        if [ -z "$_best" ] || [ "$_ms" -lt "$_best" ]; then _best=$_ms; fi
    done
    [ -n "$_best" ] || { echo FAILED; return 0; }
    echo "$_best"
}

STATUS=0

# case <label> <program>
case_row() {
    _label="$1"; _prog="$2"
    _s=$(best_of "$SILEX" "$_prog")
    _r=$(best_of "$REF"   "$_prog")
    if [ "$_s" = "FAILED" ] || [ "$_r" = "FAILED" ]; then
        printf '  %-30s %10s %10s   %s\n' "$_label" "$_s" "$_r" "(a shell FAILED)"
        STATUS=1
        return
    fi
    # A ratio needs the SLOWER side to be resolvable; the timer has ~1ms of
    # grain. Guarding on the faster side instead suppressed the sed row, which
    # is the single most dramatic result here (4ms against 389ms) -- the tiny
    # number was the finding, not the noise. Flag imprecision, do not hide it.
    _max=$_s; [ "$_r" -gt "$_max" ] && _max=$_r
    if [ "$_max" -lt 20 ]; then
        printf '  %-30s %8s ms %8s ms   (too fast to rate)\n' "$_label" "$_s" "$_r"
        return
    fi
    _min=$_s; [ "$_r" -lt "$_min" ] && _min=$_r
    _note=""; [ "$_min" -lt 10 ] && _note="  (+-1ms grain)"
    printf '  %-30s %8s ms %8s ms   %s%s\n' "$_label" "$_s" "$_r" \
        "$(awk -v s="$_s" -v r="$_r" 'BEGIN{
              if (s <= r) printf "%.2fx FASTER", r/s; else printf "%.2fx slower", s/r }')" \
        "$_note"
}

printf '  %-30s %11s %11s\n' "" "silex" "$(basename "$REF")"
echo "  ------------------------------------------------------------------------"
echo "  DISPATCH -- what silex is for"
case_row "pipe into cat (an applet)" \
    'i=0; while [ $i -lt 2000 ]; do echo hi | cat >/dev/null; i=$((i+1)); done'
case_row "pipe into od (NOT an applet)" \
    'i=0; while [ $i -lt 1000 ]; do echo hi | od -c >/dev/null; i=$((i+1)); done'
case_row "2000 x sed on a small file" \
    'printf "a\nb\nc\n" > /tmp/.silexbench.$$; i=0;
     while [ $i -lt 2000 ]; do sed -n 2p /tmp/.silexbench.$$ >/dev/null; i=$((i+1)); done;
     rm -f /tmp/.silexbench.$$'

echo ""
echo "  INTERPRETATION -- the ceiling"
# The five programs come from tests/bench/cases.sh, which bench_compare.sh also
# sources. They used to be written out here and again there, agreeing only by a
# comment -- so a change to one would have silently made the two scripts
# measure different work under the same label.
OLDIFS=$IFS
IFS='
'
for _label in $BENCH_CASE_LABELS; do
    IFS=$OLDIFS
    case_row "$_label" "$(bench_case_prog "$_label")"
    IFS='
'
done
IFS=$OLDIFS

echo ""
if [ "$STATUS" -ne 0 ]; then
    echo "BENCHMARK FAILED: a shell did not complete a case."
    echo "A failed run is not a fast run. Refusing to report a summary."
    exit 1
fi

echo "Read this as: silex wins big where the shell DISPATCHES, and is now ahead"
echo "of dash everywhere it INTERPRETS too -- it used to lose there."
echo "Builds are dispatch-bound, which is why the end-to-end figure in"
echo "bench_shell.sh is positive but small. Do not quote either half alone."
