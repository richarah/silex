#!/bin/bash
# bench_compare.sh — compare TWO silex builds against each other and dash,
# interleaved.
#
# WHY THIS EXISTS, AND WHEN TO USE IT INSTEAD OF bench_interpreter.sh:
#
# bench_interpreter.sh times one silex against dash. Comparing two silex builds
# with it means running it twice, and two sequential runs on this machine do not
# compare. Measured on 2026-08-20, the same two binaries, minutes apart:
#
#   sequential (bench_interpreter.sh, run A then run B)
#       arithmetic 100k:  base 62 ms   new 68 ms   -> "the change cost 10%"
#   interleaved (this script, same binaries, same machine)
#       arithmetic 100k:  base 64 ms   new 63 ms   -> no difference
#
# The first reading is an artifact of which build ran first. A change that
# touches nothing on that path cannot cost 10%, and did not. Machine state
# drifts over minutes -- thermal, other load, page cache -- so an A/B split
# across two runs charges that drift to whichever build ran second.
#
# So: round-robin the cases, best-of-N over the rounds, and take the minimum
# only from samples that are real. Every sample is validated because WSL2 slews
# and occasionally STEPS CLOCK_REALTIME BACKWARDS -- a negative delta would win
# a best-of comparison permanently and silently. This is the same rule
# bench_interpreter.sh's header records, for the same reason.
#
# Usage:  bench_compare.sh BASE_SILEX NEW_SILEX
# Env:    ROUNDS (default 9), REF (default dash)
#
# NOTE: each binary must be NAMED `silex`. silex dispatches on argv[0], so a
# copy called `silex-old` looks up an applet named "silex-old", finds none, and
# exits 1 -- which this script reports as a discarded sample rather than a
# result. Put each build in its own directory as `silex`.

set -u

BASE="${1:-}"
NEW="${2:-}"
REF="${REF:-$(command -v dash || command -v sh)}"
ROUNDS="${ROUNDS:-9}"

usage() { echo "usage: bench_compare.sh BASE_SILEX NEW_SILEX" >&2; exit 2; }
[ -n "$BASE" ] && [ -n "$NEW" ] || usage
for b in "$BASE" "$NEW" "$REF"; do
    [ -x "$b" ] || { echo "bench: not executable: $b" >&2; exit 1; }
done
for b in "$BASE" "$NEW"; do
    case "$(basename "$b")" in
        silex) ;;
        *) echo "bench: $b must be named 'silex' (argv[0] dispatch); see header" >&2
           exit 1 ;;
    esac
done

# Case list. The first five mirror bench_interpreter.sh's INTERPRETATION block
# so the two agree; the rest cover prefix/suffix stripping, where a literal
# pattern and a glob pattern take entirely different routes through expand.c.
LABELS=(
  "arithmetic while loop 100k"
  "case dispatch 100k"
  "test builtin 100k"
  "function call 50k"
  "parameter expansion 50k"
  "glob-pattern strip 50k"
  "greedy literal strip 20k"
  "greedy literal miss 5k"
)
PROGS=(
  'i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done'
  'i=0; while [ $i -lt 100000 ]; do case $i in *7) ;; *) ;; esac; i=$((i+1)); done'
  'i=0; while [ $i -lt 100000 ]; do [ "$i" != "zzz" ] && i=$((i+1)); done'
  'f(){ :; }; i=0; while [ $i -lt 50000 ]; do f; i=$((i+1)); done'
  'v=abcdefghij; i=0; while [ $i -lt 50000 ]; do x=${v#a}; x=${v%j}; i=$((i+1)); done'
  'v=a/b/c; i=0; while [ $i -lt 50000 ]; do x=${v##*/}; x=${v%%/*}; i=$((i+1)); done'
  'v=$(awk "BEGIN{for(i=0;i<400;i++)printf \"ab\"}"); i=0; while [ $i -lt 20000 ]; do x=${v##a}; x=${v%%b}; i=$((i+1)); done'
  'v=$(awk "BEGIN{for(i=0;i<2000;i++)printf \"ab\"}"); i=0; while [ $i -lt 5000 ]; do x=${v##zz}; x=${v%%zz}; i=$((i+1)); done'
)

ms() {
    local t0 t1 d
    t0=$(date +%s%N)
    "$1" -c "$2" >/dev/null 2>&1 || return 1
    t1=$(date +%s%N)
    d=$(( (t1 - t0) / 1000000 ))
    # A negative or non-numeric delta is a clock artifact, not a fast run.
    [ "$d" -ge 0 ] 2>/dev/null || return 1
    echo "$d"
}

declare -A BEST
discarded=0
n=${#LABELS[@]}

echo "=== interleaved A/B benchmark ==="
echo "base:      $BASE"
echo "new:       $NEW"
echo "reference: $REF"
echo "rounds:    $ROUNDS"

for _r in $(seq 1 "$ROUNDS"); do
    for ((i = 0; i < n; i++)); do
        for who in base new ref; do
            case $who in base) bin=$BASE ;; new) bin=$NEW ;; ref) bin=$REF ;; esac
            v=$(ms "$bin" "${PROGS[$i]}") || { discarded=$((discarded + 1)); continue; }
            k="$i|$who"
            if [ -z "${BEST[$k]:-}" ] || [ "$v" -lt "${BEST[$k]}" ]; then BEST[$k]=$v; fi
        done
    done
done

echo ""
printf '  %-28s %8s %8s %8s   %-16s %s\n' "" base new "$(basename "$REF")" "new vs ref" "new vs base"
echo "  --------------------------------------------------------------------------------------"
status=0
for ((i = 0; i < n; i++)); do
    b=${BEST["$i|base"]:-} w=${BEST["$i|new"]:-} d=${BEST["$i|ref"]:-}
    if [ -z "$b" ] || [ -z "$w" ] || [ -z "$d" ]; then
        printf '  %-28s %s\n' "${LABELS[$i]}" "INCOMPLETE (a shell failed every round)"
        status=1
        continue
    fi
    printf '  %-28s %6sms %6sms %6sms   %-16s %s\n' "${LABELS[$i]}" "$b" "$w" "$d" \
        "$(awk -v n="$w" -v d="$d" 'BEGIN{ if (n<=d) printf "%.2fx FASTER", d/n;
                                           else printf "%.2fx slower", n/d }')" \
        "$(awk -v n="$w" -v b="$b" 'BEGIN{ if (n<=b) printf "%.2fx faster", b/n;
                                           else printf "%.2fx SLOWER", n/b }')"
done

echo ""
echo "  discarded samples: $discarded"
if [ "$status" -ne 0 ]; then
    echo "BENCHMARK INCOMPLETE: a shell did not complete a case."
    echo "A failed run is not a fast run. Refusing to report a summary."
fi
exit "$status"
