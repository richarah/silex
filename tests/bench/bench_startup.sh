#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_startup.sh — startup latency benchmark
# chmod +x tests/bench/bench_startup.sh
#
# Usage: ./bench_startup.sh [MATCHBOX_BINARY] [ITERATIONS]
#   MATCHBOX_BINARY  path to matchbox binary (default: build/bin/matchbox)
#   ITERATIONS       number of iterations per command (default: 1000)
#
# Output: TSV format
#   command <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

MATCHBOX="${1:-build/bin/matchbox}"
N="${2:-1000}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found or not executable: $MATCHBOX" >&2
    exit 1
fi

# Check for required tools
have_cmd() { command -v "$1" > /dev/null 2>&1; }

if ! have_cmd date; then
    echo "ERROR: date command not available" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Timing: use date +%s%N for nanosecond precision on Linux
# ---------------------------------------------------------------------------

# Returns elapsed nanoseconds
bench_one() {
    local cmd="$1"
    local ts_start ts_end
    ts_start=$(date +%s%N)
    eval "$cmd" > /dev/null 2>&1
    ts_end=$(date +%s%N)
    echo $((ts_end - ts_start))
}

# Run N iterations of cmd, collect times, compute stats
# Output: mean_ms min_ms max_ms stddev_ms
bench_command() {
    local label="$1"
    local cmd="$2"
    local n="$3"
    local times_ns=()

    printf 'Benchmarking %s (%d iterations)...\n' "$label" "$n" >&2

    for i in $(seq 1 "$n"); do
        times_ns+=( "$(bench_one "$cmd")" )
    done

    # Compute stats in awk (avoid bc dependency)
    printf '%s\n' "${times_ns[@]}" | awk -v label="$label" '
    BEGIN { sum=0; min=9999999999999; max=0; n=0 }
    {
        v=$1
        vals[n++] = v
        sum += v
        if (v < min) min = v
        if (v > max) max = v
    }
    END {
        mean = sum / n
        # Variance
        var = 0
        for (i = 0; i < n; i++) {
            diff = vals[i] - mean
            var += diff * diff
        }
        if (n > 1) var /= (n - 1)
        stddev = sqrt(var)
        # Convert ns -> ms
        printf "%s\t%.4f\t%.4f\t%.4f\t%.4f\n",
            label,
            mean/1000000,
            min/1000000,
            max/1000000,
            stddev/1000000
    }'
}

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# Startup benchmark: %d iterations each\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# matchbox: %s\n' "$MATCHBOX"
printf '#\n'
printf 'command\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

# matchbox: shell startup via -c "true"
bench_command "matchbox -c true" "$MATCHBOX -c 'true'" "$N"

# dash (if available)
if have_cmd dash; then
    bench_command "dash -c true" "dash -c 'true'" "$N"
else
    printf 'dash\tSKIP\tSKIP\tSKIP\tSKIP\t# dash not installed\n'
fi

# bash
if have_cmd bash; then
    bench_command "bash -c true" "bash -c 'true'" "$N"
else
    printf 'bash\tSKIP\tSKIP\tSKIP\tSKIP\t# bash not installed\n'
fi

# sh (POSIX shell)
if have_cmd sh; then
    bench_command "sh -c true" "sh -c 'true'" "$N"
else
    printf 'sh\tSKIP\tSKIP\tSKIP\tSKIP\t# sh not installed\n'
fi

# busybox ash (if available)
if have_cmd busybox; then
    bench_command "busybox sh -c true" "busybox sh -c 'true'" "$N"
else
    printf 'busybox-sh\tSKIP\tSKIP\tSKIP\tSKIP\t# busybox not installed\n'
fi

# matchbox: echo hello (tests builtin dispatch overhead)
bench_command "matchbox echo hello" "$MATCHBOX echo hello" "$N"

# matchbox: --list (tests argument parsing with output)
bench_command "matchbox --list" "$MATCHBOX --list" "$N"

# System /bin/echo for comparison
bench_command "/bin/echo hello" "/bin/echo hello" "$N"

printf '#\n'
printf '# Done.\n'
