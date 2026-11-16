#!/bin/bash
# run_builtin_leak.sh -- resource-growth test for applets on the BUILTIN path.
#
# Usage: run_builtin_leak.sh [path/to/silex] [APPLET...]
#
# Why this exists
# ---------------
# Inside `silex sh`, an applet is not a process: it is a function call in the
# shell (see simple_cmd_applet() in src/shell/exec.c). Whatever it fails to
# free, or fails to close, stays in the shell for the rest of the script. A
# standalone `silex sed ...` hides that completely -- the kernel reclaims
# everything at exit -- which is how sed leaked a parsed script on every
# rejected program and a file descriptor on every failed `sed -i` without any
# existing suite (unit, valgrind, ASan) noticing.
#
# What it does
# ------------
# For each applet, ONE long-lived `silex sh` process runs every case for that
# applet (tests/leak/cases.sh), both success and error cases, through the
# builtin path. For each case, inside that process:
#
#   1. run the case once, keeping its output, and check its exit status
#      (ok -> 0, err -> non-zero); a case that does not take the path it
#      claims is reported as BADCASE
#   2. WARMUP more runs, unmeasured (lazy buffers, caches, stdio, ...)
#   3. sample: open fds (/proc/$$/fd) and VmRSS (/proc/$$/status), in-process
#      -- the `read` builtin and a glob, no fork, no command substitution
#   4. ITERS measured runs (window 1), sample again
#   5. ITERS measured runs (window 2), sample again
#   6. run it once more, keeping its output: exit status and output must be
#      identical to step 1 (an applet that leaves state behind in the shell
#      changes what the next invocation does) -- reported as UNSTABLE
#
# A case FAILS if it is BADCASE or UNSTABLE, or if, over the 2 x ITERS
# measured runs:
#   - the shell's open-fd count changed at all          (fd delta must be 0)
#   - VmRSS grew by more than SLACK_KB in BOTH windows  (default 16 KiB per
#     window of ITERS=1000 runs, i.e. a detection floor of ~16 bytes/run).
#     A leak grows every window; a one-off allocation (a lazily created
#     buffer, a page of heap) grows at most one, so it is not a failure.
# and an applet FAILS if any of its cases fail, or if its shell exits with a
# LeakSanitizer report (sanitised builds: see below).
#
# A CONTROL process runs the identical loop -- same redirections -- around
# `:` only, and is judged by the same rule. Its steady-state (second window)
# growth is the shell's own cost of the loop, and is SUBTRACTED from both
# windows of every applet case, so
# growth in the shell is reported once, as CONTROL, and not charged to 32
# applets. (This is how the suite found that every redirection leaked 16
# bytes into a never-reset arena until the shell abort()ed at 64 MB.)
#
# Knobs (environment):
#   LEAK_WARMUP   unmeasured runs per case before sampling     (default 50)
#   LEAK_ITERS    measured runs per window, two windows/case   (default 1000)
#   LEAK_SLACK_KB allowed VmRSS growth per window, KiB         (default 16)
#   LEAK_TIMEOUT  seconds per applet process                   (default 600)
#   LEAK_RSS      1: judge VmRSS growth; 0: report it only. Default: 0 for
#                 an ASan build (detected with nm), 1 otherwise.
#
# Sanitised builds (make test-leak-asan): VmRSS is not a usable signal there
# -- the sanitizer allocator and its shadow memory grow on their own (gcc's
# ASan: ~70 KiB per 1000 runs of the bare CONTROL loop, with nothing leaked)
# -- so the RSS rule is reported but not judged (LEAK_RSS). LeakSanitizer
# replaces it, and is exact: at each shell's exit it reports every allocation
# nobody can reach any more, with a stack, and that report fails the applet.
# (What LSan cannot see is memory still REACHABLE but growing -- the redirect
# arena records were exactly that -- which is why the release build keeps
# the RSS rule; CI runs both.) The fd and stability rules apply everywhere.
# ASan's quarantine is turned off unless ASAN_OPTIONS is already set.
#
# Applets that cannot run in-process: `sh` and `env` never return to their
# caller (sh may exit(), env execs), so simple_cmd_applet() always forks them.
# They are still run here -- the shell side of that fork path must not grow
# either -- but their own cleanup is not what is being measured.

set -uo pipefail

SILEX="${1:-build/bin/silex}"
shift || true
case "$SILEX" in
    /*) ;;
    *)  SILEX="$(cd "$(dirname "$SILEX")" && pwd)/$(basename "$SILEX")" ;;
esac
if [ ! -x "$SILEX" ]; then
    echo "ERROR: silex binary not found at '$SILEX'" >&2
    exit 1
fi
if [ ! -r /proc/self/status ] || [ ! -d /proc/self/fd ]; then
    echo "ERROR: /proc is required (fd and VmRSS sampling)" >&2
    exit 1
fi

WARMUP="${LEAK_WARMUP:-50}"
ITERS="${LEAK_ITERS:-1000}"
SLACK_KB="${LEAK_SLACK_KB:-16}"
LEAK_RSS="${LEAK_RSS:-auto}"
if [ "$LEAK_RSS" = auto ]; then
    LEAK_RSS=1
    if command -v nm >/dev/null 2>&1 && nm "$SILEX" 2>/dev/null | grep __asan_init >/dev/null; then   # not grep -q: SIGPIPE under pipefail
        LEAK_RSS=0
    fi
fi
TIMEOUT="${LEAK_TIMEOUT:-600}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:quarantine_size_mb=0:abort_on_error=0}"
# Symbolised sanitizer stacks if a symbolizer is around (not required).
if [ -z "${ASAN_SYMBOLIZER_PATH:-}" ]; then
    for _s in llvm-symbolizer llvm-symbolizer-{20,19,18,17,16,15,14}; do
        if command -v "$_s" >/dev/null 2>&1; then
            export ASAN_SYMBOLIZER_PATH="$(command -v "$_s")"; break
        fi
    done
fi
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
# Not inherited by the applets under test: keep them on their builtin path.
unset SILEX_NO_APPLETS

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=cases.sh
. "$SCRIPT_DIR/cases.sh"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/silex-leak.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# --- applet list: from the binary, so a new applet cannot be skipped --------
mapfile -t ALL_APPLETS < <("$SILEX" --list 2>/dev/null | awk 'NF{print $1}')
if [ "${#ALL_APPLETS[@]}" -eq 0 ]; then
    echo "ERROR: '$SILEX --list' printed no applets" >&2
    exit 1
fi
# Groups in cases.sh that are not applets (cross-applet scenarios) run after
# the applets.
mapfile -t EXTRA_GROUPS < <(printf '%s\n' "$LEAK_CASES" | awk -F'|' 'NF>=3{print $1}' |
    awk '!seen[$0]++' | grep -vxF -f <(printf '%s\n' "${ALL_APPLETS[@]}") || true)
if [ "$#" -gt 0 ]; then
    APPLETS=("$@")
else
    APPLETS=("${ALL_APPLETS[@]}" "${EXTRA_GROUPS[@]}")
fi

# Every applet needs an ok AND an err case.
missing=""
for a in "${ALL_APPLETS[@]}"; do
    for k in ok err; do
        grep -E "^$a\|$k(,[a-z]+)*\|" <<<"$LEAK_CASES" >/dev/null || missing="$missing $a($k)"
    done
done
if [ -n "$missing" ]; then
    echo "ERROR: tests/leak/cases.sh has no case for:$missing" >&2
    exit 1
fi

# --- fixtures ---------------------------------------------------------------
make_fixtures() {
    local w="$1"
    rm -rf "$w"; mkdir -p "$w/d" "$w/tmp"
    printf 'a:1\nb:2\nc:3\n' > "$w/f"
    printf 'a\nb\n' > "$w/g"
    : > "$w/d/x"
    : > "$w/mvA"
    ln -s f "$w/lnk0"
    printf 's/a/A/\n/b/d\n' > "$w/script.sed"
    (cd "$w" && sha256sum f > sums)
    printf '%064d  f\n' 0 > "$w/badsums"
}

# --- script generation --------------------------------------------------------
# Emits a silex sh script running each "EXPECT|COMMAND" line on stdin.
gen_script() {
    local idx=0 expect cmd
    cat <<'HDR'
_measure() {
    _fds=0
    for _f in /proc/$$/fd/*; do _fds=$((_fds + 1)); done
    _rss=0
    while read -r _k _v _u; do
        case $_k in VmRSS:) _rss=$_v ;; esac
    done < /proc/$$/status
}
HDR
    while IFS='|' read -r expect cmd; do
        idx=$((idx + 1))
        cat <<CASE
{ $cmd
} </dev/null >"$OUTDIR/$idx.first" 2>&1
_rc=\$?
_i=0
while [ \$_i -lt $WARMUP ]; do
    { $cmd
    } </dev/null >/dev/null 2>&1
    _i=\$((_i + 1))
done
_measure; _f0=\$_fds; _r0=\$_rss
_i=0
while [ \$_i -lt $ITERS ]; do
    { $cmd
    } </dev/null >/dev/null 2>&1
    _i=\$((_i + 1))
done
_measure; _r1=\$_rss
_i=0
while [ \$_i -lt $ITERS ]; do
    { $cmd
    } </dev/null >/dev/null 2>&1
    _i=\$((_i + 1))
done
_measure; _f1=\$_fds; _r2=\$_rss
{ $cmd
} </dev/null >"$OUTDIR/$idx.last" 2>&1
echo "CASE $idx $expect \$_rc \$? \$_f0 \$_f1 \$_r0 \$_r1 \$_r2"
CASE
    done
    echo 'echo "DONE"'
}

# --- run ---------------------------------------------------------------------
printf 'silex builtin-path leak test\n'
printf '  binary : %s\n' "$SILEX"
printf '  growth : per case, WARMUP=%s unmeasured runs, then 2 windows of ITERS=%s\n' "$WARMUP" "$ITERS"
printf '           measured runs, in one silex sh process per applet. FAIL if the\n'
if [ "$LEAK_RSS" = 1 ]; then
    printf '           fd count changes, or VmRSS grows > %s KiB in BOTH windows\n' "$SLACK_KB"
    printf '           (applet rows: net of CONTROL), or LeakSanitizer reports.\n'
else
    printf '           fd count changes, or LeakSanitizer/UBSan reports. VmRSS growth\n'
    printf '           is shown but NOT judged (LEAK_RSS=0: sanitised build).\n'
fi
printf '\n%-14s %-4s %4s %4s %11s %9s  %-7s %s\n' \
    APPLET KIND RC FD 'KiB W1/W2' 'B/RUN' RESULT COMMAND

total_fail=0
total_xfail=0
CTL_G1=0
CTL_G2=0
total_cases=0
failed_applets=""

run_one() {   # $1 = label, $2 = "EXPECT|COMMAND" lines
    local label="$1" lines="$2"
    local w="$WORK/$label"
    make_fixtures "$w"
    OUTDIR="$WORK/$label.cases"; mkdir -p "$OUTDIR"
    gen_script <<<"$lines" > "$WORK/$label.sh"
    local out="$WORK/$label.out" err="$WORK/$label.err"
    (cd "$w" && timeout "$TIMEOUT" "$SILEX" sh "$WORK/$label.sh" >"$out" 2>"$err")
    local prc=$?
    local afail=0 n=0
    local -a cmds
    mapfile -t cmds <<<"$lines"

    while read -r tag idx expect rc rc2 f0 f1 r0 r1 r2; do
        [ "$tag" = CASE ] || continue
        n=$((n + 1)); total_cases=$((total_cases + 1))
        local cmd="${cmds[$((idx - 1))]#*|}"
        local kind="${expect%%,*}" flags=",${expect#*,},"
        [ "$kind" = "$expect" ] && flags=","
        local fd=$((f1 - f0)) g1=$((r1 - r0)) g2=$((r2 - r1))
        if [ "$label" = CONTROL ]; then CTL_G1=$g1; CTL_G2=$g2
        else g1=$((g1 - CTL_G2)); g2=$((g2 - CTL_G2)); fi
        local gmin=$(( g1 < g2 ? g1 : g2 ))
        local bpi=$(( gmin > 0 ? gmin * 1024 / ITERS : 0 ))
        local res=PASS why="" soft=""
        if [ "$kind" = ok ] && [ "$rc" -ne 0 ]; then soft="$soft badcase(expected-rc-0)"
        elif [ "$kind" = err ] && [ "$rc" -eq 0 ]; then soft="$soft badcase(expected-rc!=0)"
        fi
        if [ "$rc" -ne "$rc2" ]; then soft="$soft unstable(rc-$rc-then-$rc2)"
        elif [[ "$flags" != *,volatile,* ]] &&
             ! cmp -s "$OUTDIR/$idx.first" "$OUTDIR/$idx.last"; then
            soft="$soft unstable(output-changed)"
        fi
        [ "$fd" -ne 0 ] && why="$why fd-leak"
        [ "$LEAK_RSS" = 1 ] && [ "$g1" -gt "$SLACK_KB" ] && [ "$g2" -gt "$SLACK_KB" ] &&
            why="$why mem-growth"
        if [[ "$flags" == *,xfail,* ]]; then
            if [ -n "$why" ]; then res=FAIL; why="$why$soft"
            elif [ -n "$soft" ]; then res=XFAIL; why="known:$soft"; total_xfail=$((total_xfail + 1))
            else res=XPASS; why="xfail case now passes: remove the marker"
            fi
        else
            why="$why$soft"
            [ -n "$why" ] && res=FAIL
        fi
        case "$res" in FAIL|XPASS) afail=1 ;; esac
        printf '%-14s %-4s %4s %4s %11s %9s  %-7s %s%s\n' \
            "$label" "$kind" "$rc" "$fd" "$g1/$g2" "$bpi" "$res" "$cmd" \
            "${why:+   [${why# }]}"
        if [[ "$soft" == *output-changed* ]]; then
            diff "$OUTDIR/$idx.first" "$OUTDIR/$idx.last" 2>/dev/null | head -6 | sed 's/^/      | /'
        fi
    done < "$out"

    local expected_n
    expected_n=$(grep -c . <<<"$lines")
    if ! grep -q '^DONE$' "$out" || [ "$n" -ne "$expected_n" ]; then
        printf '%-14s %-4s %4s %4s %11s %9s  %-7s %s\n' "$label" - "$prc" - - - \
            FAIL "shell did not finish ($n/$expected_n cases, exit $prc)"
        afail=1
    fi
    if grep -q 'LeakSanitizer\|AddressSanitizer\|runtime error:' "$err"; then
        printf '%-14s %-4s %4s %4s %11s %9s  %-7s %s\n' "$label" - "$prc" - - - \
            FAIL "sanitizer report at shell exit:"
        # The report header, each leak's size and its top frames, the summary.
        grep -E 'ERROR: |SUMMARY: |runtime error:|(Direct|Indirect) leak of|^    #[0-5] ' "$err" |
            head -60 | sed 's/^/      | /'
        afail=1
    fi
    if [ "$afail" -ne 0 ]; then
        total_fail=$((total_fail + 1))
        failed_applets="$failed_applets $label"
    fi
    return "$afail"
}

# Control: the loop and the sampling themselves must not grow.
if ! run_one CONTROL 'ok|:'; then
    echo ""
    echo "CONTROL FAILED: the shell itself grows (or leaks fds) running the bare"
    echo "loop -- no applet involved. Its growth is subtracted from the applet"
    echo "rows below, so they show only what each applet adds on top."
fi

for a in "${APPLETS[@]}"; do
    lines=$(printf '%s\n' "$LEAK_CASES" | grep -F "$a|" | grep "^$a|" | cut -d'|' -f2-)
    if [ -z "$lines" ]; then
        echo "ERROR: no cases for applet '$a'" >&2
        total_fail=$((total_fail + 1)); failed_applets="$failed_applets $a"
        continue
    fi
    run_one "$a" "$lines"
done

echo ""
echo "Cases run: $total_cases   known failures (xfail): $total_xfail   groups failing: $total_fail"
if [ "$total_fail" -gt 0 ]; then
    echo "FAILED:$failed_applets"
    exit 1
fi
echo "PASS: no fd or memory growth, and stable results, on the builtin path"
