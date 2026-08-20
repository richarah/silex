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

# `make bench` runs every tests/bench/bench_*.sh with ONE argument (the built
# silex) and fails the target if any of them exits non-zero. This script needs
# two builds and has no second one to invent, so that invocation is a SKIP, not
# a failure -- announced rather than silent, because a benchmark that quietly
# does nothing is worse than one that is missing.
if [ -n "$BASE" ] && [ -z "$NEW" ]; then
    echo "bench_compare: skipped -- this script compares TWO silex builds."
    echo "  usage: bench_compare.sh BASE_SILEX NEW_SILEX   (each named 'silex')"
    echo "  build the base into its own directory, e.g. from a worktree of the"
    echo "  commit you are comparing against, and pass both."
    exit 0
fi
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

# The case list comes from tests/bench/cases.sh, which bench_interpreter.sh
# also sources: the five interpretation workloads it reports, plus the
# strip/substitute cases. Defining them in one place is what stops the two
# scripts from measuring different work under identical labels -- see that
# file's header for why that matters more than it sounds.
BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$BENCH_DIR/cases.sh"

LABELS=()
PROGS=()
while IFS= read -r _l; do
    [ -n "$_l" ] || continue
    LABELS+=("$_l")
    PROGS+=("$(bench_case_prog "$_l")")
done <<EOF
$BENCH_CASE_LABELS
$BENCH_STRIP_LABELS
EOF

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
declare -A GOT
declare -A LOST
n=${#LABELS[@]}

echo "=== interleaved A/B benchmark ==="
echo "base:      $BASE"
echo "new:       $NEW"
echo "reference: $REF"
echo "rounds:    $ROUNDS"

# A discarded sample must be RETRIED, not simply skipped.
#
# best-of-N's minimum falls as N grows, so comparing a best-of-9 against a
# best-of-8 hands the advantage to whichever side happened to get more valid
# draws. Dropping one bad clock reading would therefore bias the very A/B this
# script exists to make honest -- and a single global "discarded: N" line at the
# bottom gives the reader no way to see which binary lost the draw. So: retry
# until each (case, binary) has its full ROUNDS of valid samples, cap the
# retries so a genuinely broken shell cannot spin forever, and report any
# shortfall per binary. bench_interpreter.sh's best_of() does the same, for the
# same reason.
cap=$((ROUNDS * 3))
for ((i = 0; i < n; i++)); do
    for who in base new ref; do
        GOT["$i|$who"]=0; LOST["$i|$who"]=0
    done
done

for _r in $(seq 1 "$ROUNDS"); do
    for ((i = 0; i < n; i++)); do
        for who in base new ref; do
            case $who in base) bin=$BASE ;; new) bin=$NEW ;; ref) bin=$REF ;; esac
            # A case the reference shell does not implement (${v//} in dash) is
            # not a slow reference and not a broken benchmark -- it has no
            # reference column. Skip the sampling rather than bank a failure.
            if [ "$who" = ref ] && bench_case_is_nonposix "${LABELS[$i]}"; then
                continue
            fi
            k="$i|$who"
            # One valid sample for this slot, retrying past clock artifacts.
            while :; do
                if v=$(ms "$bin" "${PROGS[$i]}"); then
                    GOT[$k]=$(( ${GOT[$k]} + 1 ))
                    if [ -z "${BEST[$k]:-}" ] || [ "$v" -lt "${BEST[$k]}" ]; then
                        BEST[$k]=$v
                    fi
                    break
                fi
                LOST[$k]=$(( ${LOST[$k]} + 1 ))
                [ $(( ${GOT[$k]} + ${LOST[$k]} )) -lt "$cap" ] || break
            done
        done
    done
done

echo ""
printf '  %-28s %8s %8s %8s   %-16s %s\n' "" base new "$(basename "$REF")" "new vs ref" "new vs base"
echo "  --------------------------------------------------------------------------------------"
# A ratio needs a denominator the timer can actually resolve. `date +%s%N` has
# roughly 1 ms of grain, so a fast enough case reads 0 ms and d/n is a division
# by zero -- mawk prints "inf" as though it were a result, gawk dies. Report the
# raw times and say the case is unresolvable instead of either. This is the same
# guard, for the same reason, that bench_interpreter.sh carries.
ratio() { # ratio <numerator_ms> <denominator_ms> <faster_word> <slower_word>
    if [ "$2" -eq 0 ] || [ "$1" -eq 0 ]; then echo "(too fast to rate)"; return; fi
    awk -v a="$1" -v b="$2" -v f="$3" -v s="$4" \
        'BEGIN{ if (b<=a) printf "%.2fx %s", a/b, f; else printf "%.2fx %s", b/a, s }'
}

status=0
shortfall=0
for ((i = 0; i < n; i++)); do
    b=${BEST["$i|base"]:-} w=${BEST["$i|new"]:-} d=${BEST["$i|ref"]:-}
    if bench_case_is_nonposix "${LABELS[$i]}"; then
        if [ -z "$b" ] || [ -z "$w" ]; then
            printf '  %-28s %s\n' "${LABELS[$i]}" "INCOMPLETE (a silex build failed every round)"
            status=1
            continue
        fi
        printf '  %-28s %6sms %6sms %8s   %-16s %s\n' "${LABELS[$i]}" "$b" "$w" \
            "n/a" "(not in $(basename "$REF"))" "$(ratio "$b" "$w" faster SLOWER)"
        continue
    fi
    if [ -z "$b" ] || [ -z "$w" ] || [ -z "$d" ]; then
        printf '  %-28s %s\n' "${LABELS[$i]}" "INCOMPLETE (a shell failed every round)"
        status=1
        continue
    fi
    printf '  %-28s %6sms %6sms %6sms   %-16s %s\n' "${LABELS[$i]}" "$b" "$w" "$d" \
        "$(ratio "$d" "$w" FASTER slower)" \
        "$(ratio "$b" "$w" faster SLOWER)"
done

echo ""
# Per-binary attribution: an unequal number of valid samples is exactly the bias
# the retry loop above exists to prevent, so if the cap was ever hit, say where.
for ((i = 0; i < n; i++)); do
    for who in base new ref; do
        k="$i|$who"
        if [ "$who" = ref ] && bench_case_is_nonposix "${LABELS[$i]}"; then continue; fi
        if [ "${GOT[$k]}" -lt "$ROUNDS" ]; then
            printf '  WARNING: %s got %s of %s samples on "%s" (%s discarded)\n' \
                "$who" "${GOT[$k]}" "$ROUNDS" "${LABELS[$i]}" "${LOST[$k]}"
            shortfall=1
        fi
    done
done
total_lost=0
for k in "${!LOST[@]}"; do total_lost=$((total_lost + ${LOST[$k]})); done
echo "  discarded and retried: $total_lost"
if [ "$shortfall" -ne 0 ]; then
    echo "  A best-of-N minimum falls as N rises, so the columns above are NOT"
    echo "  directly comparable where a binary is short of samples."
    status=1
fi
if [ "$status" -ne 0 ]; then
    echo "BENCHMARK INCOMPLETE: a shell did not complete a case."
    echo "A failed run is not a fast run. Refusing to report a summary."
fi
exit "$status"
