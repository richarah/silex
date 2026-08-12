#!/bin/sh
# run-oils-spec.sh — Run Oils/OSH spec tests against silex.
#
# The Oils project (formerly Oil Shell) maintains the most comprehensive
# POSIX shell spec test suite, covering edge cases, quoting, expansion,
# builtins, and control flow.
#
# The vendored test/sh_spec.py is Python-2-era; rather than modifying the
# pristine checkout, this runner copies it (plus spec_lib.py) into a temp
# dir, applies the minimal Python-3 fixes, and stubs the one Oils-internal
# module it imports (doctools.html_head, used only for HTML reports).
#
# Real usage is `sh_spec.py [options] TEST_FILE shell...` (one file per
# invocation, shells as positional args) -- the old runner passed a
# nonexistent --shell option, executed zero tests, and reported that as a
# result. ysh-*/oil-* files test the Oil language, not POSIX sh; they are
# skipped.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPOS_DIR="$SCRIPT_DIR/repos"
OIL_DIR="$REPOS_DIR/oil"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"

echo "=== Oils/OSH Spec Tests ==="
echo "Binary: $SILEX"
echo ""

[ -d "$OIL_DIR" ] || { echo "ERROR: Oils repo not found at $OIL_DIR (run fetch-all.sh)"; exit 1; }
[ -x "$SILEX" ]  || { echo "ERROR: silex binary not found: $SILEX"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 required"; exit 1; }
[ -f "$OIL_DIR/test/sh_spec.py" ] || { echo "ERROR: test/sh_spec.py missing from Oil repo"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- copy + py3-patch the runner (checkout stays pristine) ---
mkdir -p "$WORK/harness/test" "$WORK/harness/doctools" "$WORK/tmp"
cp "$OIL_DIR/test/sh_spec.py" "$OIL_DIR/test/spec_lib.py" "$WORK/harness/test/"
: > "$WORK/harness/test/__init__.py"
: > "$WORK/harness/doctools/__init__.py"
cat > "$WORK/harness/doctools/html_head.py" <<'EOF'
def Write(f, title, css_urls=None, js_urls=None):
    f.write('<html><head><title>%s</title></head>' % title)
EOF
python3 - "$WORK/harness/test/sh_spec.py" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("p.stdin.write(code)", "p.stdin.write(code.encode())")
s = s.replace("json.loads(exp_json, encoding='utf-8')", "json.loads(exp_json)")
s = s.replace(
    "            actual['stdout'], actual['stderr'] = p.communicate()",
    "            _out, _err = p.communicate()\n"
    "            actual['stdout'] = _out.decode('utf-8', 'replace') if isinstance(_out, bytes) else _out\n"
    "            actual['stderr'] = _err.decode('utf-8', 'replace') if isinstance(_err, bytes) else _err")
open(p, 'w').write(s)
EOF

PASS=0; FAIL=0; NFILES=0; ERRFILES=0
cd "$OIL_DIR"
for f in spec/*.test.sh; do
    case $f in
        spec/ysh-*|spec/oil-*|spec/hay*|spec/stateful*) continue ;;  # Oil language, not POSIX sh
    esac
    NFILES=$((NFILES + 1))
    # --stats-file gives machine-readable counters. (Scraping the ANSI
    # summary table silently loses every file whose cases all have the SAME
    # result: sh_spec.py skips "trivial" summaries, so all-pass and all-fail
    # files -- 36 of 139 -- were miscounted as "crashed".)
    rm -f "$WORK/stats"
    PYTHONPATH="$WORK/harness" timeout 90 python3 "$WORK/harness/test/sh_spec.py" \
            --tmp-env "$WORK/tmp" --path-env "$PATH" \
            --stats-file "$WORK/stats" \
            --stats-template '%(num_passed)d %(num_failed)d %(num_cases_run)d' \
            "$f" "$SILEX" >/dev/null 2>&1
    if [ ! -f "$WORK/stats" ]; then
        ERRFILES=$((ERRFILES + 1))
        echo "  no result (crash/hang): $f"
        continue
    fi
    read -r p fl _total < "$WORK/stats"
    PASS=$((PASS + ${p:-0}))
    FAIL=$((FAIL + ${fl:-0}))
done

TOTAL=$((PASS + FAIL))
echo ""
echo "=== Oils/OSH Spec Test Results ==="
echo "  Files run:    $NFILES ($ERRFILES produced no result)"
echo "  TOTAL cases:  $TOTAL"
echo "  PASS:         $PASS"
echo "  FAIL:         $FAIL"
echo ""
echo "Oils: pass=$PASS fail=$FAIL total=$TOTAL"

# The suite includes bash/ksh extension cases no POSIX shell passes; treat
# "ran a substantial number of cases" as suite success and track the ratio
# in the scorecard instead of a hard gate.
[ "$TOTAL" -ge 500 ]
