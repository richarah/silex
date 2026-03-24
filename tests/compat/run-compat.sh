#!/bin/sh
# Silex compatibility test suite
# Tests whether real-world Ubuntu/Debian Dockerfiles work on silex:slim
# with only the FROM line changed.
#
# Usage: ./run-compat.sh [test-name]
#   ./run-compat.sh           # run all tests
#   ./run-compat.sh redis     # run one test
#
# Exit code: number of failures.
# Build cache is NOT pruned between runs (re-runs are faster).

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

PASS=0
FAIL=0

TESTS="redis nginx python-numpy postgres cpython node-sharp opencv"

_run_test() {
    _name="$1"
    _df="$SCRIPT_DIR/${_name}.Dockerfile"
    _tag="silex-compat-${_name}"

    if [ ! -f "$_df" ]; then
        printf "silex compat: %-20s SKIP  (no Dockerfile)\n" "$_name"
        return
    fi

    _log=$(mktemp /tmp/silex-compat-XXXXXX.log)
    _start=$(date +%s)

    if docker build --no-cache -f "$_df" -t "$_tag" "$SCRIPT_DIR" \
            >"$_log" 2>&1; then
        _end=$(date +%s)
        printf "silex compat: %-20s PASS  (%ds)\n" "$_name" "$(( _end - _start ))"
        PASS=$(( PASS + 1 ))
    else
        _end=$(date +%s)
        printf "silex compat: %-20s FAIL  (%ds)\n" "$_name" "$(( _end - _start ))"
        printf "  --- last 30 lines of build output ---\n"
        tail -30 "$_log" | sed 's/^/  /'
        FAIL=$(( FAIL + 1 ))
    fi

    rm -f "$_log"
    docker rmi "$_tag" >/dev/null 2>&1 || true
}

if [ -n "$1" ]; then
    _run_test "$1"
else
    for _t in $TESTS; do
        _run_test "$_t"
    done
fi

_total=$(( PASS + FAIL ))
printf "\n%d/%d passed" "$PASS" "$_total"
if [ "$FAIL" -gt 0 ]; then
    printf ", %d failed" "$FAIL"
fi
printf "\n"

exit "$FAIL"
