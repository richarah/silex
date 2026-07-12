#!/bin/sh
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# bench_mkdir_deep.sh — benchmark mkdir -p where first 5 of 10 levels already exist.
# Usage: bash bench_mkdir_deep.sh <silex-binary> [iterations]
# This tests the O-08 skip-existing-prefix optimisation.

set -eu
SILEX=${1:-build/bin/silex}
ITERS=${2:-100}

TMPBASE=$(mktemp -d /tmp/bench_mkdir_deep_XXXXXX)
trap 'rm -rf "$TMPBASE"' EXIT

echo "# mkdir -p deep benchmark: $ITERS iterations"
echo "# System: $(uname -srm)"
echo "# Date:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "# silex: $SILEX"
echo "#"
echo "command	scenario	mean_ms	min_ms	max_ms	stddev_ms"

# Helper: measure command in a loop
bench() {
    local label="$1"
    local scenario="$2"
    shift 2
    local sum=0 min=999999 max=0
    local vals=""
    local i=0
    while [ "$i" -lt "$ITERS" ]; do
        # Set up: pre-create first 5 levels of a unique depth-10 path
        local base="$TMPBASE/run_${i}"
        mkdir -p "$base/l1/l2/l3/l4/l5"
        local t0 t1 ms
        t0=$(date +%s%3N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1000))')
        "$@" "$base/l1/l2/l3/l4/l5/l6/l7/l8/l9/l10" 2>/dev/null || true
        t1=$(date +%s%3N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1000))')
        ms=$((t1 - t0))
        sum=$((sum + ms))
        [ "$ms" -lt "$min" ] && min=$ms
        [ "$ms" -gt "$max" ] && max=$ms
        vals="$vals $ms"
        # Clean up created dirs
        rm -rf "$base"
        i=$((i + 1))
    done
    local mean
    mean=$(echo "$sum $ITERS" | awk '{printf "%.4f", $1/$2}')
    # stddev
    local stdev
    stdev=$(echo "$vals $ITERS $mean" | awk '{n=$NF; m=$(NF-1); for(i=1;i<NF-1;i++){d=$i-m; ss+=d*d}; printf "%.4f", sqrt(ss/n)}')
    echo "${label}	${scenario}	${mean}	${min}	${max}	${stdev}"
}

# silex mkdir -p: 5 existing levels, 5 new levels
bench "$SILEX mkdir-p" "depth10-5exist" "$SILEX" mkdir -p

# system mkdir -p for comparison
bench "system mkdir-p" "depth10-5exist" mkdir -p

echo "#"
echo "# Done."
