#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_builtin_cp.sh — cp builtin benchmark
# chmod +x tests/bench/bench_builtin_cp.sh
#
# Usage: ./bench_builtin_cp.sh [MATCHBOX_BINARY] [ITERATIONS]
#   MATCHBOX_BINARY  path to matchbox binary (default: build/bin/matchbox)
#   ITERATIONS       iterations per size (default: 1000)
#
# Creates test files of 1KB, 10KB, 100KB, 1MB.
# Benchmarks: matchbox cp vs /bin/cp
# Output: TSV format
#   command <tab> size_label <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

MATCHBOX="${1:-build/bin/matchbox}"
N="${2:-1000}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found: $MATCHBOX" >&2
    exit 1
fi

TMPDIR_CP=$(mktemp -d)
trap 'rm -rf "$TMPDIR_CP"' EXIT INT TERM

# ---------------------------------------------------------------------------
# Create test files
# ---------------------------------------------------------------------------

create_file() {
    local path="$1"
    local size_bytes="$2"
    # Fill with pseudo-random-ish content (deterministic, fast)
    dd if=/dev/urandom of="$path" bs="$size_bytes" count=1 2>/dev/null
}

SRC_1K="$TMPDIR_CP/src_1k.bin"
SRC_10K="$TMPDIR_CP/src_10k.bin"
SRC_100K="$TMPDIR_CP/src_100k.bin"
SRC_1M="$TMPDIR_CP/src_1m.bin"

printf 'Creating test files...\n' >&2
create_file "$SRC_1K"   1024
create_file "$SRC_10K"  10240
create_file "$SRC_100K" 102400
create_file "$SRC_1M"   1048576
printf 'Test files created.\n' >&2

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

# bench_cp label cmd_binary src dst n
# Runs n copies of src -> dst, removing dst before each iteration
bench_cp() {
    local label="$1"
    local cp_cmd="$2"
    local src="$3"
    local dst="$4"
    local n="$5"
    local size_label="$6"
    local times=()

    printf 'Benchmarking %s size=%s iterations=%d...\n' "$label" "$size_label" "$n" >&2

    for i in $(seq 1 "$n"); do
        rm -f "$dst"
        times+=( "$(bench_one_ns "$cp_cmd $src $dst")" )
    done
    rm -f "$dst"

    printf '%s\n' "${times[@]}" | awk -v label="$label" -v sz="$size_label" '
    BEGIN { sum=0; min=9999999999999; max=0; n=0 }
    {
        v=$1
        vals[n++] = v
        sum += v
        if (v < min) min = v
        if (v > max) max = v
    }
    END {
        if (n == 0) { print label"\t"sz"\tNO_DATA"; exit }
        mean = sum / n
        var = 0
        for (i = 0; i < n; i++) {
            diff = vals[i] - mean
            var += diff * diff
        }
        if (n > 1) var /= (n - 1)
        stddev = sqrt(var)
        printf "%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, sz,
            mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# cp benchmark: %d iterations per (command x size)\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# matchbox: %s\n' "$MATCHBOX"
printf '#\n'
printf 'command\tsize\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# matchbox cp benchmarks
# ---------------------------------------------------------------------------

for src_file in "$SRC_1K:1KB" "$SRC_10K:10KB" "$SRC_100K:100KB" "$SRC_1M:1MB"; do
    src="${src_file%%:*}"
    label="${src_file##*:}"
    dst="$TMPDIR_CP/mb_dst_${label}.bin"
    bench_cp "matchbox-cp" "$MATCHBOX cp" "$src" "$dst" "$N" "$label"
done

# ---------------------------------------------------------------------------
# /bin/cp benchmarks
# ---------------------------------------------------------------------------

SYSTEM_CP="/bin/cp"
[ ! -x "$SYSTEM_CP" ] && SYSTEM_CP=$(command -v cp 2>/dev/null || echo "")

if [ -n "$SYSTEM_CP" ] && [ -x "$SYSTEM_CP" ]; then
    for src_file in "$SRC_1K:1KB" "$SRC_10K:10KB" "$SRC_100K:100KB" "$SRC_1M:1MB"; do
        src="${src_file%%:*}"
        label="${src_file##*:}"
        dst="$TMPDIR_CP/sys_dst_${label}.bin"
        bench_cp "system-cp" "$SYSTEM_CP" "$src" "$dst" "$N" "$label"
    done
else
    for label in 1KB 10KB 100KB 1MB; do
        printf 'system-cp\t%s\tSKIP\tSKIP\tSKIP\tSKIP\t# /bin/cp not found\n' "$label"
    done
fi

# ---------------------------------------------------------------------------
# Summary: ratio matchbox/system for each size
# ---------------------------------------------------------------------------

printf '#\n'
printf '# Done.\n'
