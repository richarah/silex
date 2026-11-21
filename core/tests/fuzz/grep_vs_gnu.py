#!/usr/bin/env python3
# Differential fuzz of silex grep against GNU grep: random BRE/ERE/-F patterns
# (anchors, classes, groups, alternation, intervals, word anchors, back-references,
# invalid syntax) with many option combinations on small random inputs.
#
#   tests/fuzz/grep_vs_gnu.py [SEED [CASES]]     (make fuzz-grep-gnu runs several seeds)
#
# Needs GNU grep as `grep` on PATH and python3; not part of `make test`.
# Prints every difference in output or exit status and exits 1 if there is one.
import random, subprocess, sys, os, tempfile
os.environ['LC_ALL'] = 'C'
os.environ.pop('LC_CTYPE', None)
SILEX = os.environ.get('SILEX', 'build/bin/silex')
seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
N = int(sys.argv[2]) if len(sys.argv) > 2 else 500
random.seed(seed)
D = tempfile.mkdtemp(prefix='grep-vs-gnu-%d.' % seed)

atoms_bre = ['a', 'b', 'c', 'x', ' ', '_', '-', '.', '\\.', '[ab]', '[^a]', '[a-c]', '[[:alpha:]]', '[[:digit:]]',
             '[[:space:]]', '\\w', '\\W', '\\s', '\\S', '\\<', '\\>', '\\b', '\\B', '^', '$', '1', 'A', 'B',
             '[]a]', '[a-]', '\\(a\\)', '\\(ab\\|c\\)', '\\(\\)', '*', 'a*', 'b\\+', 'c\\?', 'a\\{2\\}', 'b\\{1,2\\}',
             '\\{,2\\}', 'x\\{0\\}', '\\|', '\\(', '\\)', '\\`', "\\'", '\\1', '[[:upper:]]', 'ab', 'abc', 'foo', 'bar']
atoms_ere = ['a', 'b', 'c', 'x', ' ', '_', '-', '.', '\\.', '[ab]', '[^a]', '[a-c]', '[[:alpha:]]', '[[:digit:]]',
             '\\w', '\\W', '\\s', '\\S', '\\<', '\\>', '\\b', '\\B', '^', '$', '1', 'A', '(a)', '(ab|c)', '()',
             '*', '+', '?', 'a*', 'b+', 'c?', 'a{2}', 'b{1,2}', '{,2}', 'x{0}', '|', '(', ')', '{', '}', 'a{1',
             '\\1', 'ab', 'abc', 'foo', 'bar', '(foo|bar)', '[[:upper:]]', 'a|b', '(a|)', 'a{0,}']
words = ['a', 'b', 'c', 'ab', 'abc', 'x', 'foo', 'bar', 'foobar', 'A', 'AB', 'aaa', 'bb', '1', '12', '_', '-', '.',
         'a.b', 'a_b', 'c-d', '', ' ', 'Foo', 'xab', 'abx', 'aXb', 'a1', 'abab', 'ba']

def gen_pat(ere):
    at = atoms_ere if ere else atoms_bre
    return ''.join(random.choice(at) for _ in range(random.randint(1, 5)))

def gen_input():
    lines = []
    for _ in range(random.randint(0, 12)):
        lines.append(' '.join(random.choice(words) for _ in range(random.randint(0, 5))))
    s = '\n'.join(lines)
    if lines and random.random() < 0.8: s += '\n'
    return s

optsets = [[], ['-i'], ['-v'], ['-c'], ['-o'], ['-w'], ['-x'], ['-n'], ['-b'], ['-ow'], ['-ox'], ['-no'], ['-ob'],
           ['--color=always'], ['-iw'], ['-vc'], ['-wc'], ['-io'], ['-l'], ['-q'], ['-m2'], ['-A1'], ['-B1'], ['-C1'],
           ['-wn'], ['-vn'], ['-xv'], ['-iwo'], ['-H'], ['-c', '-m1'], ['--color=always', '-w'], ['-vx'], ['-L']]

fails = 0
WARN = os.environ.get('WARN')
inp = os.path.join(D, 'in.txt')
for it in range(N):
    ere = random.random() < 0.5
    mode = random.random()
    if mode < 0.1:
        pats = [random.choice(words + ['a.b', 'a*', 'foo|bar'])]
        base = ['-F']
    else:
        base = ['-E'] if ere else []
        pats = [gen_pat(ere) for _ in range(1 if random.random() < 0.8 else 2)]
    opts = random.choice(optsets)
    data = gen_input()
    with open(inp, 'w') as f: f.write(data)
    args = base + opts
    for p in pats: args += ['-e', p]
    args.append(inp)
    import re as _re
    g = subprocess.run(['grep'] + args, capture_output=True)
    g.stdout = g.stdout.replace(b'\x1b[01;31m\x1b[K', b'\x1b[01;31m').replace(b'\x1b[m\x1b[K', b'\x1b[0m')
    s = subprocess.run([SILEX, 'grep'] + args, capture_output=True)
    gerr = g.returncode == 2
    serr = s.returncode == 2
    bad = False
    if gerr or serr:
        bad = gerr != serr
    else:
        bad = g.stdout != s.stdout or g.returncode != s.returncode
        # warnings
        if (b'warning' in g.stderr) != (b'warning' in s.stderr): bad = True
    if bad:
        fails += 1
        print('FAIL args=%r' % (args[:-1],))
        print('  input=%r' % data)
        print('  gnu rc=%d out=%r err=%r' % (g.returncode, g.stdout[:300], g.stderr[:200]))
        print('  slx rc=%d out=%r err=%r' % (s.returncode, s.stdout[:300], s.stderr[:200]))
        if fails > 15: break
print('seed %d: %d fails / %d' % (seed, fails, it + 1))
sys.exit(1 if fails else 0)
