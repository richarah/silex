#!/bin/bash
# tests/bench/bench_builtin_mkdir.sh — mkdir builtin benchmark
# chmod +x tests/bench/bench_builtin_mkdir.sh
#
# Usage: ./bench_builtin_mkdir.sh [MATCHBOX_BINARY] [ITERATIONS]
#   MATCHBOX_BINARY  path to matchbox binary (default: build/bin/matchbox)
#   ITERATIONS       iterations (default: 10000)
#
# Benchmarks: matchbox mkdir -p a/b/c/d/e vs /bin/mkdir -p
# Cleans up between each run.
# Output: TSV format
#   command <tab> test_case <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

MATCHBOX="${1:-build/bin/matchbox}"
N="${2:-10000}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found: $MATCHBOX" >&2
    exit 1
fi

TMPDIR_MKDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR_MKDIR"' EXIT INT TERM

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

# bench_mkdir label cmd_prefix dir_path n test_case_label
# Creates dir_path on each iteration and removes it afterwards.
bench_mkdir() {
    local label="$1"
    local mkdir_cmd="$2"
    local dir_path="$3"
    local n="$4"
    local case_label="$5"
    local times=()

    printf 'Benchmarking %s case=%s iterations=%d...\n' \
        "$label" "$case_label" "$n" >&2

    for i in $(seq 1 "$n"); do
        rm -rf "$dir_path"
        times+=( "$(bench_one_ns "$mkdir_cmd $dir_path")" )
    done
    rm -rf "$dir_path"

    printf '%s\n' "${times[@]}" | awk -v label="$label" -v cas="$case_label" '
    BEGIN { sum=0; min=9999999999999; max=0; n=0 }
    {
        v=$1
        vals[n++] = v
        sum += v
        if (v < min) min = v
        if (v > max) max = v
    }
    END {
        if (n == 0) { print label"\t"cas"\tNO_DATA"; exit }
        mean = sum / n
        var = 0
        for (i = 0; i < n; i++) {
            diff = vals[i] - mean
            var += diff * diff
        }
        if (n > 1) var /= (n - 1)
        stddev = sqrt(var)
        printf "%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, cas,
            mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

SYSTEM_MKDIR="/bin/mkdir"
[ ! -x "$SYSTEM_MKDIR" ] && SYSTEM_MKDIR=$(command -v mkdir 2>/dev/null || echo "")

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# mkdir benchmark: %d iterations per (command x case)\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# matchbox: %s\n' "$MATCHBOX"
printf '#\n'
printf 'command\ttest_case\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

# Case 1: shallow single directory
SHALLOW="$TMPDIR_MKDIR/singledir"
bench_mkdir "matchbox-mkdir" "$MATCHBOX mkdir" "$SHALLOW" "$N" "single-dir"
if [ -n "$SYSTEM_MKDIR" ] && [ -x "$SYSTEM_MKDIR" ]; then
    bench_mkdir "system-mkdir" "$SYSTEM_MKDIR" "$SHALLOW" "$N" "single-dir"
fi

# Case 2: mkdir -p deep: a/b/c/d/e (5 levels)
DEEP5="$TMPDIR_MKDIR/a/b/c/d/e"
bench_mkdir "matchbox-mkdir-p" "$MATCHBOX mkdir -p" "$DEEP5" "$N" "depth-5"
if [ -n "$SYSTEM_MKDIR" ] && [ -x "$SYSTEM_MKDIR" ]; then
    bench_mkdir "system-mkdir-p" "$SYSTEM_MKDIR -p" "$DEEP5" "$N" "depth-5"
fi

# Case 3: mkdir -p very deep: a/b/c/d/e/f/g/h/i/j (10 levels)
DEEP10="$TMPDIR_MKDIR/a/b/c/d/e/f/g/h/i/j"
bench_mkdir "matchbox-mkdir-p" "$MATCHBOX mkdir -p" "$DEEP10" "$N" "depth-10"
if [ -n "$SYSTEM_MKDIR" ] && [ -x "$SYSTEM_MKDIR" ]; then
    bench_mkdir "system-mkdir-p" "$SYSTEM_MKDIR -p" "$DEEP10" "$N" "depth-10"
fi

# Case 4: mkdir -p on already-existing path (no-op)
mkdir -p "$TMPDIR_MKDIR/existing/path"
EXISTING="$TMPDIR_MKDIR/existing/path"
bench_mkdir_existing() {
    local label="$1"
    local mkdir_cmd="$2"
    local dir_path="$3"
    local n="$4"
    local case_label="$5"
    local times=()

    printf 'Benchmarking %s case=%s iterations=%d...\n' \
        "$label" "$case_label" "$n" >&2

    # Do NOT clean up between runs: testing -p on existing dir (idempotent)
    for i in $(seq 1 "$n"); do
        times+=( "$(bench_one_ns "$mkdir_cmd $dir_path")" )
    done

    printf '%s\n' "${times[@]}" | awk -v label="$label" -v cas="$case_label" '
    BEGIN { sum=0; min=9999999999999; max=0; n=0 }
    {
        v=$1
        vals[n++] = v
        sum += v
        if (v < min) min = v
        if (v > max) max = v
    }
    END {
        if (n == 0) { print label"\t"cas"\tNO_DATA"; exit }
        mean = sum / n
        var = 0
        for (i = 0; i < n; i++) {
            diff = vals[i] - mean
            var += diff * diff
        }
        if (n > 1) var /= (n - 1)
        stddev = sqrt(var)
        printf "%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, cas,
            mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

bench_mkdir_existing "matchbox-mkdir-p" "$MATCHBOX mkdir -p" "$EXISTING" "$N" "existing-path"
if [ -n "$SYSTEM_MKDIR" ] && [ -x "$SYSTEM_MKDIR" ]; then
    bench_mkdir_existing "system-mkdir-p" "$SYSTEM_MKDIR -p" "$EXISTING" "$N" "existing-path"
fi

# Case 5: mkdir of many siblings (wide, not deep): create N dirs in sequence
bench_mkdir_many() {
    local label="$1"
    local mkdir_cmd="$2"
    local base="$3"
    local n="$4"
    local case_label="$5"
    local total_ns=0
    local ts_start ts_end

    printf 'Benchmarking %s case=%s iterations=%d...\n' \
        "$label" "$case_label" "$n" >&2

    rm -rf "$base"
    mkdir -p "$base"
    ts_start=$(date +%s%N)
    for i in $(seq 1 "$n"); do
        eval "$mkdir_cmd $base/dir_$i" > /dev/null 2>&1
    done
    ts_end=$(date +%s%N)
    total_ns=$((ts_end - ts_start))
    rm -rf "$base"

    # Per-call mean (total / n)
    awk -v label="$label" -v cas="$case_label" -v total_ns="$total_ns" -v n="$n" '
    BEGIN {
        mean = total_ns / n
        printf "%s\t%s\t%.4f\t-\t-\t-\t# total=%.2fms for %d calls\n",
            label, cas, mean/1000000, total_ns/1000000, n
    }'
}

MANY_BASE="$TMPDIR_MKDIR/many_siblings"
MANY_N=500   # Reduced from 10000 for single-call benchmark (10000 calls total)
bench_mkdir_many "matchbox-mkdir-many" "$MATCHBOX mkdir" "$MANY_BASE/mb" "$MANY_N" "many-siblings"
if [ -n "$SYSTEM_MKDIR" ] && [ -x "$SYSTEM_MKDIR" ]; then
    bench_mkdir_many "system-mkdir-many" "$SYSTEM_MKDIR" "$MANY_BASE/sys" "$MANY_N" "many-siblings"
fi

printf '#\n'
printf '# Done.\n'
