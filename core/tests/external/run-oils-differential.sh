#!/bin/sh
# run-oils-differential.sh — Oils spec suite as a GAP FINDER, not a score.
#
# Most of the Oils suite tests bash/ksh extensions that no POSIX shell
# passes, so silex's raw pass count says little. Running silex AND a
# reference POSIX shell (dash) in ONE sh_spec.py invocation and diffing
# per case separates signal from noise:
#
#     dash PASS + silex FAIL  =  a real POSIX gap        <- the work queue
#     both FAIL               =  a bash/ksh extension    <- noise
#
# Output: a per-file breakdown plus $OUT/gaps.txt listing every gap case as
#     FILE<TAB>CASE#<TAB>description
#
# Usage:  run-oils-differential.sh [FILE...]      (bare names, e.g. builtin-set)
# Env:    SILEX=path  REF=path (default dash)  OUT=dir  JOBS=n
#
# Two cautions learned the hard way:
#   * never rebuild silex while a sweep runs — this script snapshots the
#     binary to a temp dir so a concurrent `make` cannot change it midway;
#   * a sweep that silently loses files under-reports, so every selected
#     file must produce a non-empty TSV or the run is reported incomplete.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OIL_DIR="$SCRIPT_DIR/repos/oil"
SILEX="${SILEX:-$SCRIPT_DIR/../../build/bin/silex}"
REF="${REF:-$(command -v dash 2>/dev/null)}"
OUT="${OUT:-${TMPDIR:-/tmp}/silex-oils-diff}"
JOBS="${JOBS:-4}"

[ -d "$OIL_DIR" ] || { echo "ERROR: Oils repo not found at $OIL_DIR (run fetch-all.sh)"; exit 1; }
[ -x "$SILEX" ]   || { echo "ERROR: silex binary not found: $SILEX"; exit 1; }
[ -n "$REF" ] && [ -x "$REF" ] || { echo "ERROR: reference shell not found (set REF=)"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 required"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Snapshot the binary: a rebuild mid-sweep would otherwise change the
# thing being measured (and ETXTBSY every exec).
cp "$SILEX" "$WORK/silex"
SILEX_SNAP="$WORK/silex"

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

rm -rf "$OUT"; mkdir -p "$OUT/tsv"

# --- select files ---
if [ "$#" -gt 0 ]; then
    FILES=""
    for n in "$@"; do
        f="$OIL_DIR/spec/${n%.test.sh}.test.sh"
        [ -f "$f" ] || { echo "ERROR: no such spec file: $n"; exit 1; }
        FILES="$FILES ${n%.test.sh}"
    done
else
    FILES=$(cd "$OIL_DIR/spec" && for f in *.test.sh; do
        case $f in ysh-*|oil-*|hay*|stateful*) continue ;; esac   # Oil language, not POSIX sh
        echo "${f%.test.sh}"
    done)
fi

echo "=== Oils differential: silex vs $(basename "$REF") ==="
echo "silex: $SILEX"
echo "out:   $OUT"
echo ""

# --- run (each file independent; parallel by JOBS) ---
run_one() {
    name=$1
    # Each worker needs its own tmp-env: cases cd into it and write files.
    env_dir="$WORK/tmp/$name"; mkdir -p "$env_dir"
    PYTHONPATH="$WORK/harness" timeout 120 python3 "$WORK/harness/test/sh_spec.py" \
        --tmp-env "$env_dir" --path-env "$PATH" \
        --tsv-output "$OUT/tsv/$name.tsv" \
        "$OIL_DIR/spec/$name.test.sh" "$SILEX_SNAP" "$REF" >/dev/null 2>&1
}

cd "$OIL_DIR" || exit 1
n=0
for name in $FILES; do
    run_one "$name" &
    n=$((n + 1))
    [ "$((n % JOBS))" -eq 0 ] && wait
done
wait

# --- diff per case ---
python3 - "$OUT" "$OIL_DIR" $FILES <<'EOF'
import os, re, sys

out, oil_dir = sys.argv[1], sys.argv[2]
names = sys.argv[3:]

def descriptions(path):
    """case index -> the #### description line, in sh_spec.py's own order."""
    d, i = {}, 0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('####'):
            d[i] = line[4:].strip()
            i += 1
    return d

gaps, missing, total_gap = [], [], 0
per_file = []
for name in names:
    tsv = os.path.join(out, 'tsv', name + '.tsv')
    if not os.path.exists(tsv) or os.path.getsize(tsv) == 0:
        missing.append(name)
        continue
    res = {}   # case -> {shell: result}
    order = []
    with open(tsv) as f:
        next(f, None)
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != 3:
                continue
            case, shell, result = parts
            if case not in res:
                res[case] = {}
                order.append(case)
            res[case][shell] = result
    desc = descriptions(os.path.join(oil_dir, 'spec', name + '.test.sh'))
    n = 0
    for case in order:
        cells = res[case]
        shells = list(cells)
        if len(shells) < 2:
            continue
        silex, ref = cells[shells[0]], cells[shells[1]]
        # 'ok'/'BUG'/'N-I' are annotated-expected results, not clean passes.
        if ref == 'pass' and silex != 'pass':
            n += 1
            gaps.append((name, case, silex, desc.get(int(case), '')))
    if n:
        per_file.append((n, name))
    total_gap += n

per_file.sort(reverse=True)
with open(os.path.join(out, 'gaps.txt'), 'w') as f:
    for name, case, silex, d in gaps:
        f.write('%s\t%s\t%s\t%s\n' % (name, case, silex, d))

print('%-28s %s' % ('FILE', 'dash-pass silex-fail'))
for n, name in per_file:
    print('%-28s %d' % (name, n))
print('')
print('files measured: %d' % (len(names) - len(missing)))
if missing:
    print('INCOMPLETE — no TSV for: %s' % ' '.join(missing))
print('TOTAL GAPS: %d   (detail: %s/gaps.txt)' % (total_gap, out))
EOF
