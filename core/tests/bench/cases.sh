# cases.sh — the interpretation workloads, defined once.
#
# NOT named bench_*.sh on purpose: `make bench` runs every tests/bench/bench_*.sh
# as a benchmark, and this file defines functions rather than measuring
# anything — it would show up as a benchmark that silently reports nothing.
#
# Sourced by bench_interpreter.sh (silex vs a reference shell) and by
# bench_compare.sh (two silex builds vs a reference). Both used to carry their
# own copy of these five programs, agreeing only by a comment saying they
# agreed. That is the shape of a stale baseline: edit a loop bound or the
# `${v#a}` in one file and the two measure different work while both still
# print "parameter expansion 50k" — and the numbers get quoted
# interchangeably, which is exactly how EXTERNAL_TEST_SCORECARD.md came to
# carry a 1.02x figure that did not reproduce.
#
# POSIX sh, no bashisms: bench_interpreter.sh is a /bin/sh script.
#
# Defines BENCH_CASE_LABELS (newline-separated labels, in order) and, for each
# label, a function bench_case_prog <n> that echoes the n'th program. An array
# would be cleaner and is not available in POSIX sh.

# The five INTERPRETATION cases. These are the ceiling claim: no forks, no
# external commands, nothing but the shell interpreting its own syntax.
BENCH_CASE_LABELS='arithmetic while loop 100k
case dispatch 100k
test builtin 100k
function call 50k
parameter expansion 50k'

# The strip/substitute cases. Kept apart from the five above because they are
# not part of the long-standing ceiling comparison — they were added on
# 2026-08-20 with the literal fast path, to cover the shapes that path changed:
# a glob pattern (which still goes through fnmatch), a greedy literal match,
# and a greedy literal MISS, which is where the old brute force was quadratic.
BENCH_STRIP_LABELS='glob-pattern strip 50k
greedy literal strip 20k
greedy literal miss 5k
literal substitution 200x2k'

# Cases the POSIX reference shell cannot run at all.
#
# ${v//pat/repl} is a bash/ksh extension; dash exits with a syntax error, which
# a harness comparing three shells must not read as "slow" or as a broken
# benchmark. silex implements it, so the case is still worth measuring -- just
# with no reference column. Returns 0 (true) when the named case is one of
# these.
bench_case_is_nonposix() {
    case "$1" in
    'literal substitution 200x2k') return 0 ;;
    *) return 1 ;;
    esac
}

bench_case_prog() {
    case "$1" in
    'arithmetic while loop 100k')
        echo 'i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done' ;;
    'case dispatch 100k')
        echo 'i=0; while [ $i -lt 100000 ]; do case $i in *7) ;; *) ;; esac; i=$((i+1)); done' ;;
    'test builtin 100k')
        echo 'i=0; while [ $i -lt 100000 ]; do [ "$i" != "zzz" ] && i=$((i+1)); done' ;;
    'function call 50k')
        echo 'f(){ :; }; i=0; while [ $i -lt 50000 ]; do f; i=$((i+1)); done' ;;
    'parameter expansion 50k')
        echo 'v=abcdefghij; i=0; while [ $i -lt 50000 ]; do x=${v#a}; x=${v%j}; i=$((i+1)); done' ;;
    'glob-pattern strip 50k')
        echo 'v=a/b/c; i=0; while [ $i -lt 50000 ]; do x=${v##*/}; x=${v%%/*}; i=$((i+1)); done' ;;
    'greedy literal strip 20k')
        echo 'v=$(awk "BEGIN{for(i=0;i<400;i++)printf \"ab\"}"); i=0; while [ $i -lt 20000 ]; do x=${v##a}; x=${v%%b}; i=$((i+1)); done' ;;
    'greedy literal miss 5k')
        echo 'v=$(awk "BEGIN{for(i=0;i<2000;i++)printf \"ab\"}"); i=0; while [ $i -lt 5000 ]; do x=${v##zz}; x=${v%%zz}; i=$((i+1)); done' ;;
    'literal substitution 200x2k')
        echo 'v=$(awk "BEGIN{for(i=0;i<1000;i++)printf \"ab\"}"); i=0; while [ $i -lt 200 ]; do x=${v//ab/X}; i=$((i+1)); done' ;;
    *)
        echo "bench_cases: no such case: $1" >&2; return 1 ;;
    esac
}
