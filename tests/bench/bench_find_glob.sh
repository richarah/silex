#!/bin/sh
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# bench_find_glob.sh — benchmark find -name with suffix/literal/fnmatch patterns
# Usage: bash bench_find_glob.sh <matchbox-binary> [iterations]

set -eu
MATCHBOX=${1:-build/bin/matchbox}
ITERS=${2:-50}

TMPBASE=$(mktemp -d /tmp/bench_find_glob_XXXXXX)
trap 'rm -rf "$TMPBASE"' EXIT

# Build a 1000-file tree: 900 .h files and 100 .c files
TREE="$TMPBASE/tree"
mkdir -p "$TREE"
i=0
while [ "$i" -lt 900 ]; do
    touch "$TREE/file${i}.h"
    i=$((i + 1))
done
i=0
while [ "$i" -lt 100 ]; do
    touch "$TREE/src${i}.c"
    i=$((i + 1))
done

echo "# find -name glob benchmark: $ITERS iterations, 1000-file tree"
echo "# System: $(uname -srm)"
echo "# Date:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "# matchbox: $MATCHBOX"
echo "#"
echo "command	pattern_type	mean_ms	min_ms	max_ms	stddev_ms"

bench() {
    local label="$1"
    local ptype="$2"
    shift 2
    local sum=0 min=999999 max=0 vals="" i=0
    while [ "$i" -lt "$ITERS" ]; do
        local t0 t1 ms
        t0=$(date +%s%3N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1000))')
        "$@" > /dev/null 2>&1 || true
        t1=$(date +%s%3N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1000))')
        ms=$((t1 - t0))
        sum=$((sum + ms))
        [ "$ms" -lt "$min" ] && min=$ms
        [ "$ms" -gt "$max" ] && max=$ms
        vals="$vals $ms"
        i=$((i + 1))
    done
    local mean stdev
    mean=$(echo "$sum $ITERS" | awk '{printf "%.4f", $1/$2}')
    stdev=$(echo "$vals $ITERS $mean" | awk '{n=$NF; m=$(NF-1); for(i=1;i<NF-1;i++){d=$i-m; ss+=d*d}; printf "%.4f", sqrt(ss/n)}')
    echo "${label}	${ptype}	${mean}	${min}	${max}	${stdev}"
}

# matchbox: suffix pattern (*.c) — CGLOB_SUFFIX fast path
bench "$MATCHBOX find" "suffix-*.c"  "$MATCHBOX" find "$TREE" -name "*.c"
# matchbox: literal pattern (file0.h) — CGLOB_LITERAL fast path
bench "$MATCHBOX find" "literal"     "$MATCHBOX" find "$TREE" -name "file0.h"
# matchbox: full fnmatch ([sf]*.c) — CGLOB_FULL path
bench "$MATCHBOX find" "fnmatch"     "$MATCHBOX" find "$TREE" -name "[sf]*.c"
# system find for comparison
bench "system find"    "suffix-*.c"  find "$TREE" -name "*.c"

echo "#"
echo "# Done."
