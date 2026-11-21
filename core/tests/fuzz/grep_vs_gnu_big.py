#!/usr/bin/env python3
# Differential fuzz of silex grep against GNU grep on larger inputs: buffer refills,
# very long lines, stdin and pipes, several files, NUL bytes.
#
#   tests/fuzz/grep_vs_gnu_big.py [SEED [CASES]]
#
# Files with NUL bytes are left out unless GREP_FUZZ_NUL=1: silex and GNU grep word their
# binary-file results differently.
#
# Needs GNU grep as `grep` on PATH and python3; not part of `make test`.
# Differential test on larger inputs: buffer refills, long lines, stdin,
# pipes, several files, NUL bytes.
import random, subprocess, sys, os, tempfile
os.environ['LC_ALL'] = 'C'
os.environ.pop('LC_CTYPE', None)
SILEX = os.environ.get('SILEX', 'build/bin/silex')
seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
random.seed(seed)
D = tempfile.mkdtemp(prefix='grep-vs-gnu-%d.' % seed)
words = ['foo', 'bar', 'baz', 'error', 'warning', 'Hello', 'World', 'x1', 'abc', 'q', 'zz', 'the', 'a.b', '_id',
         '12', '3.14', 'GET', '/api/v2', 'status=200', 'x', 'y', 'xy', 'ab', 'aaa', '-', '.', ' ']

def mkfile(path, nlines, longp=0.0, nulp=0.0, term=True):
    out = []
    for i in range(nlines):
        if random.random() < longp:
            n = random.randint(1000, 400000)
        else:
            n = random.randint(0, 15)
        line = ' '.join(random.choice(words) for _ in range(n))
        if random.random() < nulp:
            k = random.randint(0, len(line))
            line = line[:k] + '\0' + line[k:]
        out.append(line)
    s = '\n'.join(out)
    if term and out: s += '\n'
    with open(path, 'w') as f: f.write(s)

files = []
for k in range(4):
    p = os.path.join(D, 'f%d' % k)
    mkfile(p, random.choice([0, 1, 50, 5000, 40000]), longp=random.choice([0, 0, 0.001, 0.01]),
           nulp=(random.choice([0, 0, 0, 0.0005]) if os.environ.get('GREP_FUZZ_NUL') else 0), term=random.random() < 0.8)
    files.append(p)

pats = ['foo', 'error', 'zz', 'x[0-9]+', 'Hello World', 'the$', '^GET', '\\bab\\b', 'a.b', '(foo|bar) baz',
        '[A-Z][a-z]+', 'q', '', 'x*', '.', 'status=[0-9]{3}', 'warning|error|zz', '\\<x', 'y\\>', 'aaa+',
        '(ab)\\1', 'nomatchzzz', '3\\.14', '^$', '^.{20,}$']
opts = [[], ['-v'], ['-c'], ['-n'], ['-o'], ['-w'], ['-x'], ['-i'], ['-l'], ['-L'], ['-q'], ['-m', '3'], ['-b'],
        ['-on'], ['-ob'], ['-vn'], ['-cv'], ['-h'], ['-H'], ['-A', '2'], ['-B', '1'], ['-C', '1'], ['-iw'], ['-wo'],
        ['-m', '1', '-c'], ['-vc'], ['-nb'], ['-F'], ['-Fi'], ['-Fw'], ['-Fx'], ['-Fv'], ['-Fc']]
fails = 0
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
for it in range(N):
    pat = random.choice(pats)
    o = random.choice(opts)
    if pat in ('.', 'x*', '') and any('o' in f[1:] for f in o if f.startswith('-')):
        continue          # one output line per byte: gigabytes on the large inputs
    mode = random.choice(['file', 'files', 'stdin', 'pipe'])
    args = ['-E'] + o + ['-e', pat]
    if '-F' in ''.join(o): args = o + ['-e', pat]
    if mode == 'file':
        cmd = args + [random.choice(files)]
        stdin = None
    elif mode == 'files':
        cmd = args + random.sample(files, random.randint(2, 4))
        stdin = None
    else:
        cmd = args
        stdin = random.choice(files)
    def run(prog):
        if mode == 'pipe':
            c = 'cat %s | %s' % (stdin, ' '.join(_q(x) for x in prog + cmd))
            return subprocess.run(c, shell=True, capture_output=True)
        f = open(stdin, 'rb') if stdin else None
        return subprocess.run(prog + cmd, stdin=f, capture_output=True)
    def _q(x):
        return "'" + x.replace("'", "'\\''") + "'"
    g = run(['grep'])
    s = run([SILEX, 'grep'])
    gout = g.stdout
    sout = s.stdout
    # binary-file handling differs by design (see docs): skip those cases
    if b'binary file matches' in g.stdout or b'Binary file' in s.stdout:
        continue
    if gout != sout or g.returncode != s.returncode:
        fails += 1
        print('FAIL mode=%s cmd=%r stdin=%r' % (mode, cmd, stdin))
        print('  gnu rc=%d len=%d  slx rc=%d len=%d' % (g.returncode, len(gout), s.returncode, len(sout)))
        a = gout.split(b'\n'); b = sout.split(b'\n')
        for i in range(min(len(a), len(b))):
            if a[i] != b[i]:
                print('  first diff line %d: gnu=%r slx=%r' % (i, a[i][:120], b[i][:120]))
                break
        if fails > 10: break
print('bigfuzz seed %d: %d fails / %d' % (seed, fails, it + 1))
sys.exit(1 if fails else 0)
