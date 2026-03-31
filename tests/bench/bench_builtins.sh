#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_builtins.sh — coreutil builtins benchmark
#
# Benchmarks key coreutil builtins (wc, sort, sed, tr, cut, awk)
# against their system equivalents.
#
# Usage: ./bench_builtins.sh [SILEX_BINARY] [ITERATIONS]
#   SILEX_BINARY  path to silex binary (default: build/bin/silex)
#   ITERATIONS       number of iterations (default: 300)
#
# Output: TSV format
#   command <tab> builtin <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

SILEX="${1:-build/bin/silex}"
N="${2:-300}"

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found: $SILEX" >&2
    exit 1
fi

TMPDIR_BUILTINS=$(mktemp -d)
trap 'rm -rf "$TMPDIR_BUILTINS"' EXIT INT TERM

# Generate a deterministic 10000-line input file
printf 'Generating test input...\n' >&2
for i in $(seq 1 10000); do
    printf '%05d alpha beta gamma delta epsilon zeta eta\n' "$i"
done > "$TMPDIR_BUILTINS/input.txt"
printf 'Test input: %s bytes, %d lines\n' \
    "$(wc -c < "$TMPDIR_BUILTINS/input.txt")" \
    "$(wc -l < "$TMPDIR_BUILTINS/input.txt")" >&2

# Shuffled version for sort benchmark
sort -R "$TMPDIR_BUILTINS/input.txt" > "$TMPDIR_BUILTINS/shuffled.txt" 2>/dev/null || \
    cp "$TMPDIR_BUILTINS/input.txt" "$TMPDIR_BUILTINS/shuffled.txt"

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
    local builtin="$2"
    local cmd="$3"
    local n="$4"
    local times=()

    printf 'Benchmarking %s/%s (%d iterations)...\n' "$label" "$builtin" "$n" >&2

    for i in $(seq 1 "$n"); do
        times+=( "$(bench_one_ns "$cmd")" )
    done

    printf '%s\n' "${times[@]}" | awk -v label="$label" -v bi="$builtin" '
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
        printf "%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label, bi, mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

INPUT="$TMPDIR_BUILTINS/input.txt"
SHUFFLED="$TMPDIR_BUILTINS/shuffled.txt"

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# builtins benchmark: %d iterations each\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# silex: %s\n' "$SILEX"
printf '#\n'
printf 'command\tbuiltin\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# wc -l
# ---------------------------------------------------------------------------
bench_command "silex" "wc-l" \
    "\"$SILEX\" wc -l \"$INPUT\"" "$N"
if command -v wc > /dev/null 2>&1; then
    bench_command "system" "wc-l" "wc -l \"$INPUT\"" "$N"
fi

# ---------------------------------------------------------------------------
# sort
# ---------------------------------------------------------------------------
bench_command "silex" "sort" \
    "\"$SILEX\" sort \"$SHUFFLED\"" "$N"
if command -v sort > /dev/null 2>&1; then
    bench_command "system" "sort" "sort \"$SHUFFLED\"" "$N"
fi

# ---------------------------------------------------------------------------
# sed s///
# ---------------------------------------------------------------------------
bench_command "silex" "sed-subst" \
    "\"$SILEX\" sed 's/alpha/ALPHA/g' \"$INPUT\"" "$N"
if command -v sed > /dev/null 2>&1; then
    bench_command "system" "sed-subst" "sed 's/alpha/ALPHA/g' \"$INPUT\"" "$N"
fi

# ---------------------------------------------------------------------------
# tr
# ---------------------------------------------------------------------------
bench_command "silex" "tr-upper" \
    "\"$SILEX\" sh -c '\"$SILEX\" cat \"$INPUT\" | \"$SILEX\" tr a-z A-Z'" "$N"
if command -v tr > /dev/null 2>&1; then
    bench_command "system" "tr-upper" "tr a-z A-Z < \"$INPUT\"" "$N"
fi

# ---------------------------------------------------------------------------
# cut
# ---------------------------------------------------------------------------
bench_command "silex" "cut-f2" \
    "\"$SILEX\" cut -d' ' -f2 \"$INPUT\"" "$N"
if command -v cut > /dev/null 2>&1; then
    bench_command "system" "cut-f2" "cut -d' ' -f2 \"$INPUT\"" "$N"
fi

# ---------------------------------------------------------------------------
# grep -c (line count mode, exercises line scanning without output)
# ---------------------------------------------------------------------------
bench_command "silex" "grep-count" \
    "\"$SILEX\" grep -c 'alpha' \"$INPUT\"" "$N"
if command -v grep > /dev/null 2>&1; then
    bench_command "system" "grep-count" "grep -c 'alpha' \"$INPUT\"" "$N"
fi

# ---------------------------------------------------------------------------
# echo (shell builtin dispatch overhead)
# ---------------------------------------------------------------------------
bench_command "silex" "echo" \
    "\"$SILEX\" echo hello world" "$N"
if command -v echo > /dev/null 2>&1; then
    bench_command "system" "echo" "/bin/echo hello world" "$N"
fi

printf '#\n'
printf '# Done.\n'
