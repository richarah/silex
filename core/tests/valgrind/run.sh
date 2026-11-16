#!/bin/bash
# run.sh -- `make test-valgrind`: the suites, with every SILEX process under
# valgrind memcheck.
#
# Usage: run.sh path/to/silex      (an UNSANITISED build: valgrind cannot
#                                   run an ASan binary; make test-valgrind
#                                   builds -O1 -g for readable stacks)
#
# What runs
#   1. the unit + shell suites (tests/unit/test_*.sh, tests/unit/shell/
#      test_*.sh -- the same set run_tests.sh globs), each handed
#      tests/valgrind/silex-vg in place of the binary, so every silex they
#      start runs under memcheck. Suites named in VG_SKIP are not run, and
#      the output says so. Default: shell/test_resource_limits.sh, whose
#      stress cases (e.g. growing "$@" to 20000 words one at a time, O(n^2))
#      each run into their own 600 s timeout at memcheck's ~30x slowdown:
#      the run would take hours and fail on timeouts, not memory errors.
#      A suite whose ONLY failing cases are listed in VG_ARTIFACTS (below)
#      counts as passed, and the artifacts are named in the output.
#   2. tests/leak/run_builtin_leak.sh, the same way, at low repetition
#      (LEAK_ITERS=VG_LEAK_ITERS, default 3): one long-lived silex sh per
#      applet running every case through the builtin path, so memcheck's
#      end-of-process leak check covers what applets leave behind in a
#      shell. Its RSS rule is off (LEAK_RSS=0: valgrind's own heap dominates
#      RSS); its fd and stability rules still apply.
#
# Verdict: FAIL if a suite failed, if any silex process logged a memcheck
# error or a definitely/indirectly lost block at exit, or if any process it
# forked logged a memcheck error (see the verdict below for why a forked
# child's exit-time leak report does not count). Logs: $VG_LOG_DIR.
#
# The old target, `valgrind bash tests/unit/run_tests.sh`, put BASH under
# valgrind. Without --trace-children it never instrumented silex at all, so
# it could not have seen a single silex leak.

set -uo pipefail

BIN="${1:?usage: run.sh path/to/silex}"
case "$BIN" in /*) ;; *) BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" ;; esac
[ -x "$BIN" ] || { echo "ERROR: no silex binary at $BIN" >&2; exit 1; }
command -v valgrind >/dev/null 2>&1 || { echo "ERROR: valgrind not installed" >&2; exit 1; }
if nm "$BIN" 2>/dev/null | grep __asan_init >/dev/null; then  # not -q: SIGPIPE + pipefail
    echo "ERROR: $BIN is an ASan build; valgrind cannot run it" >&2
    exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
# VG_VERDICT_ONLY=1 VG_LOG_DIR=DIR: re-judge an earlier run's logs, run nothing.
VG_VERDICT_ONLY="${VG_VERDICT_ONLY:-0}"
CORE="$(cd "$HERE/../.." && pwd)"
LOG="${VG_LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/silex-vg.XXXXXX")}"
mkdir -p "$LOG"
[ "$VG_VERDICT_ONLY" = 1 ] || rm -f "$LOG"/vg.*.log

export SILEX_VG_BIN="$BIN" SILEX_VG_LOG="$LOG"
WRAP="$HERE/silex-vg"
rc=0

if [ "$VG_VERDICT_ONLY" != 1 ]; then
VG_SKIP="${VG_SKIP-shell/test_resource_limits.sh}"

# Cases that fail BECAUSE of valgrind, one "suite|case" per line. Each needs
# its reason here; anything else failing still fails the run.
#
#   test_jobs "kill %n signals the job" (`sleep 5 & kill %1` must give 143):
#   silex blocks all signals across the fork and the child unblocks them
#   just before exec -- so a SIGTERM sent in that window stays pending and
#   kills the job (natively: 143, every time). Under valgrind the client's
#   signal mask is emulated and a signal arriving while it is blocked is
#   queued INSIDE valgrind, and that queue does not survive execve(): the
#   signal is dropped, sleep runs to completion, 0. A job that does not
#   exec (`(while :; do :; done) &`) gets 143 under valgrind as well.
VG_ARTIFACTS="${VG_ARTIFACTS-shell/test_jobs.sh|kill %n signals the job}"
echo "=== valgrind: unit + shell suites (silex under memcheck) ==="
pass=0; failed=""; skipped=""
for suite in "$CORE"/tests/unit/test_*.sh "$CORE"/tests/unit/shell/test_*.sh; do
    [ -f "$suite" ] || continue
    name="${suite#"$CORE/tests/unit/"}"
    case " $VG_SKIP " in
        *" $name "*) skipped="$skipped $name"; continue ;;
    esac
    echo "--- $name ---"
    out="$LOG/suite.out"
    bash "$suite" "$WRAP" 2>&1 | tee "$out"
    if [ "${PIPESTATUS[0]}" -eq 0 ]; then
        pass=$((pass + 1))
        continue
    fi
    # Failed: only listed valgrind artifacts?
    unexplained=$(sed -n 's/^FAIL: //p' "$out" | while IFS= read -r c; do
        printf '%s\n' "$VG_ARTIFACTS" | grep -qxF "$name|$c" || printf '%s\n' "$c"
    done)
    if [ -z "$unexplained" ] && grep -q '^FAIL: ' "$out"; then
        pass=$((pass + 1))
        echo "($name: failures are known valgrind artifacts, see VG_ARTIFACTS:"
        sed -n 's/^FAIL: /   /p' "$out"
        echo ")"
    else
        failed="$failed $name"; rc=1
    fi
done
echo "unit + shell suites under valgrind: $pass passed${failed:+, FAILED:$failed}"
[ -n "$skipped" ] && echo "SKIPPED under valgrind (VG_SKIP):$skipped"

echo ""
echo "=== valgrind: builtin-path leak suite (silex under memcheck) ==="
LEAK_ITERS="${VG_LEAK_ITERS:-3}" LEAK_WARMUP=1 LEAK_RSS=0 \
    bash "$CORE/tests/leak/run_builtin_leak.sh" "$WRAP" ||
    { echo "valgrind: builtin leak suite FAILED"; rc=1; }
fi   # VG_VERDICT_ONLY

echo ""
echo "=== valgrind verdict ==="
# One log per silex the suites started, vg.PID.log. Lines from that process
# are prefixed ==PID==; lines with any other pid come from its forked
# children (subshells, $(...), pipeline stages, background jobs).
#   - the process itself: any memcheck error, or a definitely/indirectly lost
#     block at exit, fails the run;
#   - a forked child: memory ERRORS fail the run, but not its exit-time leak
#     report. A child exit()s from deep inside whatever forked it -- the
#     parent's frames (an expansion holding a strndup()ed word, say) are
#     abandoned mid-flight, and memcheck calls their blocks lost. That is
#     the fork, not a leak, and nothing accumulates: the process is gone.
# A child killed by a CRASH signal counts too; one killed by SIGTERM/SIGKILL/
# SIGINT/SIGPIPE is a test doing its job (`sleep 5 & kill %1`), not a fault.
CHILD_ERR='(Invalid (read|write|free)|Mismatched free|Conditional jump|Use of uninitialised|Syscall param|Source and destination overlap|Argument .* of function|Process terminating with default action of signal (4|5|6|7|8|11) )'
nlogs=0; nchild=0; bad=""
for f in "$LOG"/vg.*.log; do
    [ -f "$f" ] || continue
    nlogs=$((nlogs + 1))
    top="${f##*/vg.}"; top="${top%.log}"
    if grep -qE "^==$top== ERROR SUMMARY: [1-9]" "$f"; then
        bad="$bad $f:$top"
    fi
    for pid in $(grep -oE '^==[0-9]+==' "$f" | tr -d = | sort -u); do
        [ "$pid" = "$top" ] && continue
        nchild=$((nchild + 1))
        if grep -qE "^==$pid== $CHILD_ERR" "$f"; then
            bad="$bad $f:$pid"
        fi
    done
done
echo "silex processes checked: $nlogs, plus $nchild forked children   (logs: $LOG)"
if [ "$nlogs" -eq 0 ]; then
    echo "FAIL: no valgrind logs -- silex never ran under valgrind"
    exit 1
fi
if [ -n "$bad" ]; then
    echo "FAIL: memcheck errors in $(printf '%s\n' $bad | wc -l) process(es):"
    for fp in $bad; do
        f="${fp%:*}"; pid="${fp##*:}"
        echo "--- $f (pid $pid)"
        grep -E "^==$pid== (Command|$CHILD_ERR|[0-9,]+ bytes in|   (at|by) |ERROR SUMMARY)" "$f" | head -40
    done
    rc=1
fi
[ "$rc" -eq 0 ] && echo "PASS: no memcheck errors or leaks in any silex process"
exit "$rc"
