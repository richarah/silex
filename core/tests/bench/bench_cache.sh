#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_cache.sh — fscache benchmark
#
# Measures the effect of the path-keyed fscache on repeated stat() calls.
# Compares: TTL=0 (cache disabled) vs TTL=5 (cache enabled, default).
#
# Usage: ./bench_cache.sh [SILEX_BINARY] [ITERATIONS]
#   SILEX_BINARY  path to silex binary (default: build/bin/silex)
#   ITERATIONS       number of iterations (default: 500)
#
# Output: TSV format
#   scenario <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

SILEX="${1:-build/bin/silex}"
N="${2:-500}"

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found or not executable: $SILEX" >&2
    exit 1
fi

TMPDIR_CACHE=$(mktemp -d)
trap 'rm -rf "$TMPDIR_CACHE"' EXIT INT TERM

# Create a directory tree with 200 files for repeated stat workload
printf 'Creating test tree...\n' >&2
for i in $(seq 1 200); do
    printf 'line %d\n' "$i" > "$TMPDIR_CACHE/file_$(printf '%04d' "$i").txt"
done
# Nested dirs
mkdir -p "$TMPDIR_CACHE/a/b/c/d/e"
for i in $(seq 1 20); do
    touch "$TMPDIR_CACHE/a/b/c/d/e/deep_$i.txt"
done
printf 'Test tree created (%d files).\n' "$(find "$TMPDIR_CACHE" -type f | wc -l)" >&2

# ---------------------------------------------------------------------------
# Timing helper
# ---------------------------------------------------------------------------

bench_one_ns() {
    local ts_start ts_end
    ts_start=$(date +%s%N)
    eval "$1" > /dev/null 2>&1
    ts_end=$(date +%s%N)
    echo $((ts_end - ts_start))
}

bench_command() {
    local label="$1"
    local cmd="$2"
    local n="$3"
    local times=()

    printf 'Benchmarking %s (%d iterations)...\n' "$label" "$n" >&2

    for i in $(seq 1 "$n"); do
        times+=( "$(bench_one_ns "$cmd")" )
    done

    printf '%s\n' "${times[@]}" | awk -v label="$label" '
    BEGIN { sum=0; min=9999999999999; max=0; n=0 }
    {
        v=$1; vals[n++]=v; sum+=v
        if (v < min) min=v
        if (v > max) max=v
    }
    END {
        mean=sum/n; var=0
        for (i=0;i<n;i++) { diff=vals[i]-mean; var+=diff*diff }
        if (n>1) var/=(n-1)
        stddev=sqrt(var)
        printf "%s\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

# The workload: ls -la on a directory tree (triggers many stat() calls)
WORKLOAD_CMD="\"$SILEX\" sh -c 'find \"$TMPDIR_CACHE\" -name \"*.txt\" | while read f; do \"$SILEX\" sh -c \"ls -la \\\"$TMPDIR_CACHE\\\"\" > /dev/null; done'"

# Simplified workload: ls on the flat directory (known number of stats)
FLAT_WORKLOAD="\"$SILEX\" sh -c 'for f in \"$TMPDIR_CACHE\"/*.txt; do \"$SILEX\" sh -c \"test -f \\\"\$f\\\"\"; done'"

# Use a grep workload to exercise repeated file stat + read
GREP_WORKLOAD_CACHED="\"$SILEX\" grep -r 'line 1' \"$TMPDIR_CACHE\""
GREP_WORKLOAD_NOCACHE="SILEX_FSCACHE_TTL=0 \"$SILEX\" grep -r 'line 1' \"$TMPDIR_CACHE\""

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# fscache benchmark: %d iterations each\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# silex: %s\n' "$SILEX"
printf '# testdir: %d files in %s\n' "$(find "$TMPDIR_CACHE" -type f | wc -l)" "$TMPDIR_CACHE"
printf '#\n'
printf 'scenario\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

# grep -r with cache enabled (default TTL=5s)
bench_command "grep-r-cache-on(TTL=5)" \
    "\"$SILEX\" grep -r 'line 1' \"$TMPDIR_CACHE\"" "$N"

# grep -r with cache disabled (TTL=0)
bench_command "grep-r-cache-off(TTL=0)" \
    "SILEX_FSCACHE_TTL=0 \"$SILEX\" grep -r 'line 1' \"$TMPDIR_CACHE\"" "$N"

# cp workload: repeated copy of the same source (exercises stat on src)
SRC="$TMPDIR_CACHE/file_0001.txt"
DST="$TMPDIR_CACHE/dst.txt"
bench_command "cp-cache-on(TTL=5)" \
    "\"$SILEX\" cp \"$SRC\" \"$DST\"" "$N"

bench_command "cp-cache-off(TTL=0)" \
    "SILEX_FSCACHE_TTL=0 \"$SILEX\" cp \"$SRC\" \"$DST\"" "$N"

printf '#\n'
printf '# Done.\n'
