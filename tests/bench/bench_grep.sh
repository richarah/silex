#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_grep.sh — grep builtin benchmark
#
# Measures grep performance for fixed-string and regex patterns over
# various file sizes, compared to system grep.
#
# Usage: ./bench_grep.sh [MATCHBOX_BINARY] [ITERATIONS]
#   MATCHBOX_BINARY  path to matchbox binary (default: build/bin/matchbox)
#   ITERATIONS       number of iterations per scenario (default: 200)
#
# Output: TSV format
#   command <tab> scenario <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

MATCHBOX="${1:-build/bin/matchbox}"
N="${2:-200}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found: $MATCHBOX" >&2
    exit 1
fi

TMPDIR_GREP=$(mktemp -d)
trap 'rm -rf "$TMPDIR_GREP"' EXIT INT TERM

# ---------------------------------------------------------------------------
# Generate deterministic test files
# ---------------------------------------------------------------------------

printf 'Generating test files...\n' >&2

# 10K lines: typical shell/config file size
python3 -c "
import sys
for i in range(10000):
    print('line %d: foo=bar baz=qux hello world matchbox pattern_here' % i)
" > "$TMPDIR_GREP/10k.txt" 2>/dev/null || {
    # Fallback without python3
    for i in $(seq 1 10000); do
        printf 'line %d: foo=bar baz=qux hello world matchbox pattern_here\n' "$i"
    done > "$TMPDIR_GREP/10k.txt"
}

# 100K lines: large log file
python3 -c "
import sys
for i in range(100000):
    print('2024-01-01T%02d:%02d:%02d INFO line %d: foo=bar baz=qux hello matchbox' % (i//3600%24, i//60%60, i%60, i))
" > "$TMPDIR_GREP/100k.txt" 2>/dev/null || {
    for i in $(seq 1 100000); do
        printf '2024-01-01 INFO line %d: foo=bar baz=qux hello matchbox\n' "$i"
    done > "$TMPDIR_GREP/100k.txt"
}

# Binary-like file with some text
dd if=/dev/urandom bs=102400 count=1 2>/dev/null | \
    tr '\000-\037\177-\377' 'A-Za-z0-9 \n' > "$TMPDIR_GREP/binary.bin"

printf 'Test files created: 10k=%s 100k=%s\n' \
    "$(wc -c < "$TMPDIR_GREP/10k.txt")B" \
    "$(wc -c < "$TMPDIR_GREP/100k.txt")B" >&2

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
    local scenario="$4"
    local times=()

    printf 'Benchmarking %s/%s (%d iterations)...\n' "$label" "$scenario" "$n" >&2

    for i in $(seq 1 "$n"); do
        times+=( "$(bench_one_ns "$cmd")" )
    done

    printf '%s\n' "${times[@]}" | awk -v label="$label" -v sc="$scenario" '
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
            label, sc, mean/1000000, min/1000000, max/1000000, stddev/1000000
    }'
}

# Find system grep
SGREP=$(command -v grep 2>/dev/null || echo "")

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# grep benchmark: %d iterations per scenario\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# matchbox: %s\n' "$MATCHBOX"
printf '#\n'
printf 'command\tscenario\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# Fixed-string benchmarks (-F)
# ---------------------------------------------------------------------------

for size_tag in "10k:$TMPDIR_GREP/10k.txt" "100k:$TMPDIR_GREP/100k.txt"; do
    tag="${size_tag%%:*}"
    file="${size_tag##*:}"

    bench_command "matchbox-grep" \
        "\"$MATCHBOX\" grep -F 'matchbox' \"$file\"" \
        "$N" "fixed-str-${tag}"

    if [ -n "$SGREP" ]; then
        bench_command "system-grep" \
            "\"$SGREP\" -F 'matchbox' \"$file\"" \
            "$N" "fixed-str-${tag}"
    fi

    # -i case-insensitive fixed string
    bench_command "matchbox-grep" \
        "\"$MATCHBOX\" grep -Fi 'MATCHBOX' \"$file\"" \
        "$N" "fixed-icase-${tag}"

    if [ -n "$SGREP" ]; then
        bench_command "system-grep" \
            "\"$SGREP\" -Fi 'MATCHBOX' \"$file\"" \
            "$N" "fixed-icase-${tag}"
    fi
done

# ---------------------------------------------------------------------------
# Regex benchmarks (BRE/ERE)
# ---------------------------------------------------------------------------

for size_tag in "10k:$TMPDIR_GREP/10k.txt" "100k:$TMPDIR_GREP/100k.txt"; do
    tag="${size_tag%%:*}"
    file="${size_tag##*:}"

    bench_command "matchbox-grep" \
        "\"$MATCHBOX\" grep 'line [0-9][0-9]*:' \"$file\"" \
        "$N" "bre-${tag}"

    if [ -n "$SGREP" ]; then
        bench_command "system-grep" \
            "\"$SGREP\" 'line [0-9][0-9]*:' \"$file\"" \
            "$N" "bre-${tag}"
    fi

    bench_command "matchbox-grep" \
        "\"$MATCHBOX\" grep -E 'foo=(bar|baz)' \"$file\"" \
        "$N" "ere-${tag}"

    if [ -n "$SGREP" ]; then
        bench_command "system-grep" \
            "\"$SGREP\" -E 'foo=(bar|baz)' \"$file\"" \
            "$N" "ere-${tag}"
    fi
done

# ---------------------------------------------------------------------------
# No-match (worst case for grep: scans entire file)
# ---------------------------------------------------------------------------

bench_command "matchbox-grep" \
    "\"$MATCHBOX\" grep -F 'xyzzy_no_match_here' \"$TMPDIR_GREP/100k.txt\"" \
    "$N" "no-match-100k"

if [ -n "$SGREP" ]; then
    bench_command "system-grep" \
        "\"$SGREP\" -F 'xyzzy_no_match_here' \"$TMPDIR_GREP/100k.txt\"" \
        "$N" "no-match-100k"
fi

printf '#\n'
printf '# Done.\n'
