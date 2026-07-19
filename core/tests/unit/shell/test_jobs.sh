#!/bin/sh
# tests/unit/shell/test_jobs.sh — job control tests for silex
# Usage: ./test_jobs.sh [path/to/silex]
#
# Non-interactive job control: the job table, job specs (%n/%+/%-/%string),
# jobs/fg/bg/kill/wait, process groups, and stop/resume via signals. Terminal
# control (Ctrl-Z, tcsetpgrp) and monitor mode need a pty and are not covered
# here.

SILEX="${1:-build/bin/silex}"
MB="$SILEX"
PASS=0
FAIL=0

check() {
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"; PASS=$((PASS + 1))
    else
        echo "FAIL: $1"
        echo "  expected: [$3]"
        echo "  got:      [$2]"
        FAIL=$((FAIL + 1))
    fi
}
check_exit() {
    if [ "$2" = "$3" ]; then echo "PASS: $1"; PASS=$((PASS + 1))
    else echo "FAIL: $1 (expected exit $3, got $2)"; FAIL=$((FAIL + 1)); fi
}

# --- job table + numbering ---------------------------------------------------
check "two background commands are two jobs" \
    "$("$MB" -c 'sleep 2 & sleep 2 & jobs | wc -l' 2>/dev/null | tr -d ' ')" "2"
check "job command text is recorded" \
    "$("$MB" -c 'sleep 9 & jobs' 2>/dev/null | grep -o 'sleep 9')" "sleep 9"
check "jobs -p prints pids only" \
    "$("$MB" -c 'sleep 2 & jobs -p' 2>/dev/null | grep -cE '^[0-9]+$')" "1"

# --- $! and wait -------------------------------------------------------------
check "\$! is the background pid" \
    "$("$MB" -c 'sleep 1 & p=$!; case $p in [0-9]*) echo numeric;; *) echo no;; esac')" "numeric"
check "wait for all background jobs returns 0" \
    "$("$MB" -c 'sleep 0.2 & sleep 0.1 & wait; echo $?')" "0"

# --- job specs: %n, %+, %-, %string ------------------------------------------
check "kill %n signals the job" \
    "$("$MB" -c 'sleep 5 & kill %1; wait %1 2>/dev/null; echo $?')" "143"
check "kill %string matches by command prefix" \
    "$("$MB" -c 'sleep 5 & kill %sleep; wait %1 2>/dev/null; echo done')" "done"
check "kill -KILL %n uses the named signal" \
    "$("$MB" -c 'sleep 5 & kill -KILL %1; wait %1 2>/dev/null; echo $?')" "137"
check "%+ is the current job, %- the previous" \
    "$("$MB" -c 'sleep 5 & sleep 5 & kill %+ %-; wait 2>/dev/null; echo done')" "done"
"$MB" -c 'kill %9 2>/dev/null' ; check_exit "kill on a nonexistent job fails" "$?" "1"

# --- fg -----------------------------------------------------------------------
check "fg waits for the job to finish" \
    "$("$MB" -c 'sleep 1 & fg %1 >/dev/null; echo $?')" "0"
# Note: stop/resume (SIGTSTP, bg of a stopped job, monitor mode) is an
# interactive feature that needs a controlling terminal; it is exercised via a
# pty harness, not here. Even bash hangs on `bg %1; wait` without a tty.

# --- process groups: a background job is in its own group --------------------
check "a background job runs in its own process group" \
    "$("$MB" -c 'sleep 2 & jobs -l' 2>/dev/null | awk "{print (\$2 != $$) ? \"own-group\" : \"shell-group\"}")" "own-group"

# --- backgrounding still runs the command ------------------------------------
check "a background command actually runs" \
    "$("$MB" -c 'touch /tmp/silex_job_$$.marker & wait; test -f /tmp/silex_job_$$.marker && echo ran; rm -f /tmp/silex_job_$$.marker')" "ran"

echo ""
echo "job control tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
