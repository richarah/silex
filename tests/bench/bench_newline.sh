#!/bin/bash
# tests/bench/bench_newline.sh — newline scanning benchmark
#
# Measures wc -l performance on a large file (100MB equivalent via /dev/urandom
# piped through tr to create ASCII text with newlines at a known density).
# Compares matchbox wc -l vs system wc -l.
#
# Usage: ./bench_newline.sh [MATCHBOX_BINARY] [ITERATIONS]

set -uo pipefail

MATCHBOX="${1:-build/bin/matchbox}"
N="${2:-20}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found: $MATCHBOX" >&2
    exit 1
fi

TMPDIR_NL=$(mktemp -d)
trap 'rm -rf "$TMPDIR_NL"' EXIT INT TERM

# Create test files
printf 'Creating test files...\n' >&2
# 1MB file: ~10k lines
python3 -c "
import sys
lines = 10000
chars_per_line = 100
for _ in range(lines):
    sys.stdout.write('a' * chars_per_line + '\n')
" > "$TMPDIR_NL/file_1mb.txt" 2>/dev/null || \
    awk 'BEGIN { for(i=1;i<=10000;i++) { printf "%100s\n","" } }' \
    > "$TMPDIR_NL/file_1mb.txt"

# 10MB file
python3 -c "
import sys
lines = 100000
chars_per_line = 100
for _ in range(lines):
    sys.stdout.write('a' * chars_per_line + '\n')
" > "$TMPDIR_NL/file_10mb.txt" 2>/dev/null || \
    awk 'BEGIN { for(i=1;i<=100000;i++) { printf "%100s\n","" } }' \
    > "$TMPDIR_NL/file_10mb.txt"
printf 'Files created.\n' >&2

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
    printf 'Benchmarking %s (%d iter)...\n' "$label" "$n" >&2
    for i in $(seq 1 "$n"); do
        times+=( "$(bench_one_ns "$cmd")" )
    done
    printf '%s\n' "${times[@]}" | awk -v label="$label" '
    BEGIN { sum=0; min=9999999999999; max=0; n=0 }
    { v=$1; vals[n++]=v; sum+=v; if(v<min)min=v; if(v>max)max=v }
    END {
        mean=sum/n; var=0
        for(i=0;i<n;i++){diff=vals[i]-mean;var+=diff*diff}
        if(n>1)var/=(n-1); stddev=sqrt(var)
        printf "%s\t%.4f\t%.4f\t%.4f\t%.4f\n",label,mean/1e6,min/1e6,max/1e6,stddev/1e6
    }'
}

printf '# newline scan benchmark: %d iterations\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# matchbox: %s\n' "$MATCHBOX"
printf '#\n'
printf 'command\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

bench_command "matchbox-wc-l-1mb"  "\"$MATCHBOX\" wc -l \"$TMPDIR_NL/file_1mb.txt\""  "$N"
bench_command "system-wc-l-1mb"    "wc -l \"$TMPDIR_NL/file_1mb.txt\""                 "$N"
bench_command "matchbox-wc-l-10mb" "\"$MATCHBOX\" wc -l \"$TMPDIR_NL/file_10mb.txt\"" "$N"
bench_command "system-wc-l-10mb"   "wc -l \"$TMPDIR_NL/file_10mb.txt\""                "$N"

printf '#\n# Done.\n'
