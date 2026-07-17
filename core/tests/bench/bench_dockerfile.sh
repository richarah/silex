#!/bin/bash
ulimit -v 4194304   # 4 GB virtual memory per process
ulimit -f 1048576   # 1 GB max file size
ulimit -t 300       # 5 min CPU time per process
ulimit -d 2097152   # 2 GB data segment
# tests/bench/bench_dockerfile.sh — Dockerfile RUN instruction simulation benchmark
#
# Simulates execution of typical Dockerfile RUN instruction patterns
# using silex as the shell runtime, measuring throughput relative
# to dash/bash.
#
# Usage: ./bench_dockerfile.sh [SILEX_BINARY] [ITERATIONS]
#   SILEX_BINARY  path to silex binary (default: build/bin/silex)
#   ITERATIONS       number of iterations per scenario (default: 200)
#
# Output: TSV format
#   command <tab> scenario <tab> mean_ms <tab> min_ms <tab> max_ms <tab> stddev_ms

set -uo pipefail

SILEX="${1:-build/bin/silex}"
N="${2:-200}"

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found: $SILEX" >&2
    exit 1
fi

TMPDIR_DF=$(mktemp -d)
trap 'rm -rf "$TMPDIR_DF"' EXIT INT TERM

# Create a fake package directory structure used in RUN patterns
mkdir -p "$TMPDIR_DF/etc/apt/lists"
mkdir -p "$TMPDIR_DF/var/cache/apt/archives"
mkdir -p "$TMPDIR_DF/usr/local/bin"
mkdir -p "$TMPDIR_DF/app/src"
for i in $(seq 1 50); do
    printf '#!/bin/sh\necho package-%d\n' "$i" > "$TMPDIR_DF/usr/local/bin/pkg_$i"
    chmod 755 "$TMPDIR_DF/usr/local/bin/pkg_$i"
    printf '{"name":"pkg-%d","version":"1.0"}\n' "$i" > "$TMPDIR_DF/app/src/pkg_$i.json"
done

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
    local scenario="$2"
    local cmd="$3"
    local n="$4"
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

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

printf '# dockerfile simulation benchmark: %d iterations each\n' "$N"
printf '# System: %s\n' "$(uname -srm)"
printf '# Date:   %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf '# silex: %s\n' "$SILEX"
printf '#\n'
printf 'command\tscenario\tmean_ms\tmin_ms\tmax_ms\tstddev_ms\n'

# ---------------------------------------------------------------------------
# Pattern 1: mkdir -p + copy (typical RUN mkdir && cp)
# ---------------------------------------------------------------------------
MKDIR_CP='
set -e
mkdir -p "'"$TMPDIR_DF"'/run_test/a/b/c"
cp "'"$TMPDIR_DF"'/app/src/pkg_1.json" "'"$TMPDIR_DF"'/run_test/a/b/c/out.json"
rm -rf "'"$TMPDIR_DF"'/run_test"
'
bench_command "silex-sh" "mkdir-cp-rm" \
    "\"$SILEX\" sh -c '$MKDIR_CP'" "$N"
if command -v dash > /dev/null 2>&1; then
    bench_command "dash" "mkdir-cp-rm" \
        "dash -c '$MKDIR_CP'" "$N"
fi
if command -v bash > /dev/null 2>&1; then
    bench_command "bash" "mkdir-cp-rm" \
        "bash -c '$MKDIR_CP'" "$N"
fi

# ---------------------------------------------------------------------------
# Pattern 2: find + grep (package list filter)
# ---------------------------------------------------------------------------
FIND_GREP='find "'"$TMPDIR_DF"'/app/src" -name "*.json" | '"$SILEX"' grep "pkg-[13579]"'
bench_command "silex-sh" "find-grep-json" \
    "\"$SILEX\" sh -c '$FIND_GREP'" "$N"
if command -v dash > /dev/null 2>&1; then
    bench_command "dash" "find-grep-json" \
        "dash -c 'find \"$TMPDIR_DF/app/src\" -name \"*.json\" | grep \"pkg-[13579]\"'" "$N"
fi

# ---------------------------------------------------------------------------
# Pattern 3: chmod -R (recursive permission set)
# ---------------------------------------------------------------------------
CHMOD_R='chmod -R 755 "'"$TMPDIR_DF"'/usr/local/bin"'
bench_command "silex-sh" "chmod-r-bin" \
    "\"$SILEX\" sh -c '$CHMOD_R'" "$N"
if command -v dash > /dev/null 2>&1; then
    bench_command "dash" "chmod-r-bin" \
        "dash -c '$CHMOD_R'" "$N"
fi

# ---------------------------------------------------------------------------
# Pattern 4: Shell script with conditionals and loops (apt-get simulation)
# ---------------------------------------------------------------------------
APT_SIM='
set -e
for pkg in libssl-dev zlib1g-dev libcurl4-openssl-dev; do
    if [ -d "'"$TMPDIR_DF"'/etc/apt/lists" ]; then
        mkdir -p "'"$TMPDIR_DF"'/var/cache/apt/archives"
        touch "'"$TMPDIR_DF"'/var/cache/apt/archives/${pkg}_1.0_amd64.deb"
    fi
done
find "'"$TMPDIR_DF"'/var/cache/apt/archives" -name "*.deb" -exec rm -f {} +
rm -rf "'"$TMPDIR_DF"'/etc/apt/lists"/*
mkdir -p "'"$TMPDIR_DF"'/etc/apt/lists"
'
bench_command "silex-sh" "apt-sim" \
    "\"$SILEX\" sh -c '$APT_SIM'" "$N"
if command -v dash > /dev/null 2>&1; then
    bench_command "dash" "apt-sim" \
        "dash -c '$APT_SIM'" "$N"
fi
if command -v bash > /dev/null 2>&1; then
    bench_command "bash" "apt-sim" \
        "bash -c '$APT_SIM'" "$N"
fi

# ---------------------------------------------------------------------------
# Pattern 5: sed substitution on multiple files
# ---------------------------------------------------------------------------
SED_MULTI='
for f in "'"$TMPDIR_DF"'/app/src"/*.json; do
    '"$SILEX"' sed "s/version/ver/g" "$f" > /dev/null
done
'
bench_command "silex-sh" "sed-multi-file" \
    "\"$SILEX\" sh -c '$SED_MULTI'" "$N"
if command -v dash > /dev/null 2>&1; then
    bench_command "dash" "sed-multi-file" \
        "dash -c 'for f in \"$TMPDIR_DF/app/src\"/*.json; do sed \"s/version/ver/g\" \"\$f\" > /dev/null; done'" "$N"
fi

printf '#\n'
printf '# Done.\n'
