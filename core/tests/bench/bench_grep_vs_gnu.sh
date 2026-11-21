#!/bin/bash
# tests/bench/bench_grep_vs_gnu.sh — silex grep against the system (GNU) grep
#
# The measurement behind docs/OPTIMISATIONS.md O-22..O-30. A ~50 MB, 800k-line
# synthetic log is generated (deterministic: awk's srand(42)), and every case
# is run as `grep ARGS FILE > OUT` for both greps, alternating, best of N.
#
# OUT is a regular file on purpose: GNU grep (and, since O-28, silex) stop at
# the first match when stdout is /dev/null, so timing `> /dev/null` -- which
# is what hyperfine does by default -- measures how early the first match is,
# not the search. Set GREP_BENCH_OUT=/dev/null to see that case.
#
# Every case also compares the two outputs and exit statuses (OK/DIFF).
# Ratio limits checked: 1.5 for fixed strings, 2.0 for regexes.
#
# Usage: bench_grep_vs_gnu.sh [SILEX_BINARY] [ITERATIONS]

SILEX="${1:-build/bin/silex}"
N="${2:-5}"
export LC_ALL=C

if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found: $SILEX" >&2
    exit 1
fi
GNU=$(command -v grep)

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM
F="$W/big.txt"
OUT="${GREP_BENCH_OUT:-$W/out}"

awk 'BEGIN {
    srand(42)
    n = split("error warning info debug request response user server client timeout connection failed success database query cache memory disk network process thread kernel module config value Hello World foo bar baz qux alpha beta gamma delta 192.168.1.1 10.0.0.254 GET POST /api/v1/users /index.html status=200 status=404 status=500 id=12345 the and of to in is that for it with as was on", w, " ")
    cs = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    for (i = 0; i < 800000; i++) {
        k = 3 + int(rand() * 12); line = ""
        if (rand() < 0.3)
            line = sprintf("2024-%02d-%02d %02d:%02d:%02d ", 1 + int(rand() * 12),
                           1 + int(rand() * 28), int(rand() * 24), int(rand() * 60), int(rand() * 60))
        for (j = 0; j < k; j++) line = line (j ? " " : "") w[1 + int(rand() * n)]
        if (rand() < 0.05) {
            s = ""; m = 5 + int(rand() * 16)
            for (j = 0; j < m; j++) s = s substr(cs, 1 + int(rand() * 62), 1)
            line = line " " s
        }
        print line
    }
}' > "$F"

now() { date +%s%N; }
best() {   # best wall time in ms of N runs of "$@" > OUT
    local b=999999999 s e t i
    for ((i = 0; i < N; i++)); do
        s=$(now); "$@" "$F" > "$OUT" 2>/dev/null; e=$(now)
        t=$(( (e - s) / 1000 )); [ "$t" -lt "$b" ] && b=$t
    done
    echo "$b"
}

printf '%-18s %10s %10s %6s %s\n' case gnu_ms silex_ms ratio check
fails=0
run() {
    local kind="$1" name="$2"; shift 2
    local go so gr sr
    go=$("$GNU" "$@" "$F" 2>&1 | cksum); gr=${PIPESTATUS[0]}
    so=$("$SILEX" grep "$@" "$F" 2>&1 | cksum); sr=${PIPESTATUS[0]}
    local tg=0 ts=0
    tg=$(best "$GNU" "$@"); ts=$(best "$SILEX" grep "$@")
    local verdict=OK
    [ "$go" = "$so" ] && [ "$gr" = "$sr" ] || verdict=DIFF
    local lim=200; [ "$kind" = F ] && lim=150
    local r=$(( ts * 100 / (tg > 0 ? tg : 1) ))
    [ "$r" -gt "$lim" ] && verdict="$verdict SLOW"
    case "$verdict" in OK) ;; *) fails=$((fails + 1)) ;; esac
    awk -v n="$name" -v g="$tg" -v s="$ts" -v v="$verdict" \
        'BEGIN { printf "%-18s %10.1f %10.1f %6.2f %s\n", n, g / 1000, s / 1000, s / (g > 0 ? g : 1), v }'
}

run F fixed          timeout
run F fixed-F        -F 'connection failed'
run F fixed-i        -i 'hello world'
run F fixed-c        -c kernel
run F fixed-v        -v the
run F fixed-w        -w id
run F fixed-o        -o status=404
run F fixed-nomatch  zzyzx
run F fixed-multi-F  -F -e timeout -e kernel -e zzyzx
run R bre-dotstar    'err.*time'
run R bre-interval   '[0-9]\{3\}\.[0-9]'
run R bre-anchor     '^2024-0[1-3]'
run R bre-class      'status=[45]0[0-9]'
run R bre-group      'fail\(ed\)* da'
run R bre-eol        'the$'
run R ere-alt-dotstar -E '(error|warning) .*timeout'
run R ere-alt-lit    -E 'foo|bar|baz'
run R ere-alt-rare   -E 'zzyzx|qwerty|Hello World'
run R ere-plus       -E 'GET /api/v[0-9]+/users'
run R ere-eol        -E 'id=[0-9]+$'
run R ere-word       -E '[A-Za-z]+[0-9]+[A-Za-z]+'
run R ere-ip         -E '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'
run R ere-nomatch    -E 'x[0-9]+y'
run R re-i           -i 'hello.*world'
run R re-i-E         -iE '(error|warning) +tim'
run R re-v           -v 'e.*r'
run R re-vE          -vE 'a|e|i'
run R re-c           -c 'q[a-z]*x'
run R re-cE          -cE '(foo|bar) (baz|qux)'
run R re-o           -o '[0-9][0-9]*'
run R re-oE          -oE 'status=[0-9]+'
run R re-oE-alt      -oE 'foo|bar'
run R re-w           -w 'fo*'
run R re-wE          -wE '[a-z]+ing'
run R re-wo          -woE '[a-z]{3}'
run R re-x           -xE '[a-z ]+'
run R re-n           -n 'kern.l mod'
run R re-empty-star  -c 'x*'
run R re-dot         -c .
run R re-start-class -cE '[A-Z][a-z]+ [A-Z]'
run R re-iw          -iw hello
run R re-backref     -E '(foo) \1'

echo "cases failing a check: $fails"
[ "$fails" -eq 0 ]
