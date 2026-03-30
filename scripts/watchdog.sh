#!/bin/sh
# scripts/watchdog.sh — resource watchdog for long-running test/bench sessions
#
# Usage: watchdog.sh PID [MAX_RSS_MB] [MAX_WALL_SEC]
#   PID          Process to watch (and its children)
#   MAX_RSS_MB   Kill if RSS exceeds this (default: 8192 = 8 GB)
#   MAX_WALL_SEC Kill if runtime exceeds this (default: 1800 = 30 min)
#
# Starts a background polling loop; exits 0 if PID finishes naturally,
# exits 1 if the watchdog killed the process due to resource excess.
#
# Example (wrap a bench run):
#   bash tests/bench/bench_startup.sh build/bin/matchbox &
#   BENCH_PID=$!
#   sh scripts/watchdog.sh $BENCH_PID 4096 600 &
#   WD_PID=$!
#   wait $BENCH_PID
#   kill $WD_PID 2>/dev/null

set -eu

TARGET_PID="${1:-}"
MAX_RSS_MB="${2:-8192}"
MAX_WALL_SEC="${3:-1800}"

if [ -z "$TARGET_PID" ]; then
    printf 'Usage: %s PID [MAX_RSS_MB] [MAX_WALL_SEC]\n' "$0" >&2
    exit 2
fi

# Verify PID exists
if ! kill -0 "$TARGET_PID" 2>/dev/null; then
    printf 'watchdog: PID %s does not exist\n' "$TARGET_PID" >&2
    exit 1
fi

START_TIME=$(date +%s)
INTERVAL=2   # poll every 2 seconds

printf 'watchdog: monitoring PID %s (max_rss=%sMB max_wall=%ss)\n' \
    "$TARGET_PID" "$MAX_RSS_MB" "$MAX_WALL_SEC" >&2

while kill -0 "$TARGET_PID" 2>/dev/null; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - START_TIME))

    # Wall time check
    if [ "$ELAPSED" -ge "$MAX_WALL_SEC" ]; then
        printf 'watchdog: KILL PID %s — wall time %ds >= limit %ds\n' \
            "$TARGET_PID" "$ELAPSED" "$MAX_WALL_SEC" >&2
        kill -TERM "$TARGET_PID" 2>/dev/null || true
        sleep 2
        kill -KILL "$TARGET_PID" 2>/dev/null || true
        exit 1
    fi

    # RSS check: sum RSS of PID and all descendants (in kB from /proc)
    RSS_KB=0
    for pid in $(pgrep -P "$TARGET_PID" 2>/dev/null) "$TARGET_PID"; do
        PROC_RSS=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
        RSS_KB=$((RSS_KB + PROC_RSS))
    done
    RSS_MB=$((RSS_KB / 1024))

    if [ "$RSS_MB" -ge "$MAX_RSS_MB" ]; then
        printf 'watchdog: KILL PID %s — RSS %dMB >= limit %dMB\n' \
            "$TARGET_PID" "$RSS_MB" "$MAX_RSS_MB" >&2
        kill -TERM "$TARGET_PID" 2>/dev/null || true
        sleep 2
        kill -KILL "$TARGET_PID" 2>/dev/null || true
        exit 1
    fi

    sleep "$INTERVAL"
done

printf 'watchdog: PID %s exited normally after %ds\n' \
    "$TARGET_PID" "$(($(date +%s) - START_TIME))" >&2
exit 0
