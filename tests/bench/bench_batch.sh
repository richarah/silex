#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_batch.sh — batch operation benchmark (io_uring path)
#
# Measures batch rm (BATCH_RM_FILE) performance: matchbox batch-rm vs
# sequential shell loop, exercising the io_uring acceleration path.
#
# Usage: ./bench_batch.sh [MATCHBOX_BINARY] [ITERATIONS]
#   MATCHBOX_BINARY  path to matchbox binary (default: build/bin/matchbox)
#   ITERATIONS       number of outer iterations (default: 100)
#
# Each iteration creates N_FILES files, then removes them via the
# batch rm path or a sequential loop, measuring wall time.
#
# Output: TSV format
#   method <tab> n_files <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

MATCHBOX="${1:-build/bin/matchbox}"
N="${2:-100}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found: $MATCHBOX" >&2
    exit 1
fi

TMPDIR_BATCH=$(mktemp -d)
trap 'rm -rf "$TMPDIR_BATCH"' EXIT INT TERM

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

bench_scenario() {
    local label="$1"
    local n_files="$2"
    local setup_cmd="$3"  # creates files
    local run_cmd="$4"    # removes files
    local n="$5"
    local times=()

    printf 'Benchmarking %s nfiles=%d (%d iterations)...\n' \
        "$label" "$n_files" "$n" >&2

    for i in $(seq 1 "$n"); do
        eval "$setup_cmd" > /dev/null 2>&1
        times+=( "$(bench_one_ns "$run_cmd")" )
    done

    printf '%s\n' "${times[@]}" | awk -v label="$label" -v nf="$n_files" '
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
        printf "%s\t%d\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, nf, mean/1000000, min/1000000, max/1000000, stddev/1000000
    }' 2>/dev/null || \
    printf '%s\n' "${times[@]}" | awk -v label="$label" -v nf="$n_files" '
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
        printf "%s\t%d\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, nf, mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

BATCH_DIR="$TMPDIR_BATCH/batchdir"

make_files() {
    local n="$1"
    mkdir -p "$BATCH_DIR"
    for i in $(seq 1 "$n"); do
        touch "$BATCH_DIR/file_$(printf '%05d' "$i")"
    done
}

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# batch benchmark: %d iterations per scenario\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# matchbox: %s\n' "$MATCHBOX"
printf '#\n'
printf 'method\tn_files\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# Benchmarks: matchbox rm vs system rm vs shell loop
# ---------------------------------------------------------------------------

for n_files in 10 50 100 200; do
    # matchbox rm (exercises batch_exec / io_uring path)
    bench_scenario "matchbox-rm" "$n_files" \
        "make_files $n_files" \
        "\"$MATCHBOX\" rm -f \"$BATCH_DIR\"/file_*" \
        "$N"

    # system rm (for comparison)
    if command -v rm > /dev/null 2>&1; then
        bench_scenario "system-rm" "$n_files" \
            "make_files $n_files" \
            "rm -f \"$BATCH_DIR\"/file_*" \
            "$N"
    fi

    # matchbox sh -c loop (sequential via shell)
    bench_scenario "matchbox-sh-loop" "$n_files" \
        "make_files $n_files" \
        "\"$MATCHBOX\" sh -c 'for f in \"$BATCH_DIR\"/file_*; do rm -f \"\$f\"; done'" \
        "$N"
done

# ---------------------------------------------------------------------------
# mkdir batch: create many directories
# ---------------------------------------------------------------------------

rm_batch_dir() {
    rm -rf "$BATCH_DIR"
}

for n_dirs in 10 50 100; do
    # matchbox mkdir -p
    bench_scenario "matchbox-mkdir-p" "$n_dirs" \
        "rm_batch_dir" \
        "for i in \$(seq 1 $n_dirs); do \"$MATCHBOX\" mkdir -p \"$BATCH_DIR/d\$(printf '%04d' \$i)/sub\"; done" \
        "$N"

    if command -v mkdir > /dev/null 2>&1; then
        bench_scenario "system-mkdir-p" "$n_dirs" \
            "rm_batch_dir" \
            "for i in \$(seq 1 $n_dirs); do mkdir -p \"$BATCH_DIR/d\$(printf '%04d' \$i)/sub\"; done" \
            "$N"
    fi
done

printf '#\n'
printf '# Done.\n'
