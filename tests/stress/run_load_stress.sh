#!/bin/sh
# tests/stress/run_load_stress.sh — run tests once under CPU load
# Spawns background CPU load, runs tests, cleans up.

MATCHBOX="${1:-build/bin/matchbox}"

if [ ! -x "$MATCHBOX" ]; then
    echo "ERROR: matchbox binary not found at $MATCHBOX"
    exit 1
fi

# Determine number of CPUs
NCPU=$(nproc 2>/dev/null || echo 2)
echo "Running tests under ${NCPU}-core CPU load..."

# Spawn background load workers
LOAD_PIDS=""
i=0
while [ "$i" -lt "$NCPU" ]; do
    yes > /dev/null &
    LOAD_PIDS="$LOAD_PIDS $!"
    i=$(( i + 1 ))
done

# Run tests under load
RC=0
if ! bash tests/unit/run_tests.sh "$MATCHBOX" >/dev/null 2>&1; then
    echo "FAIL: unit tests under load"
    RC=1
fi
if [ "$RC" -eq 0 ]; then
    if ! bash tests/integration/run_integration.sh "$MATCHBOX" >/dev/null 2>&1; then
        echo "FAIL: integration tests under load"
        RC=1
    fi
fi

# Kill load workers
for pid in $LOAD_PIDS; do
    kill "$pid" 2>/dev/null || true
done
wait 2>/dev/null || true

if [ "$RC" -eq 0 ]; then
    echo "PASS: tests under CPU load"
fi
exit "$RC"
