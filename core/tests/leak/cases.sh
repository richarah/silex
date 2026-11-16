# shellcheck shell=bash
# cases.sh -- builtin-path leak cases, one or more per applet.
#
# Sourced by run_builtin_leak.sh. Each line of LEAK_CASES is
#
#     APPLET|EXPECT|COMMAND
#
#   APPLET   the applet the case is charged to (one silex sh process each)
#   EXPECT   ok   -> COMMAND must exit 0
#            err  -> COMMAND must exit non-zero (exercises the error path)
#            optionally followed by flags, comma-separated:
#              ,volatile  output legitimately differs between runs (random
#                         names, ...): do not compare first/last output
#              ,xfail     KNOWN BUG in rc/output stability across repeated
#                         runs (documented next to the case). Reported as
#                         XFAIL; if it starts passing it is XPASS and fails
#                         the run, so the marker gets removed. fd and memory
#                         growth are NEVER waived by xfail.
#   COMMAND  shell text, run verbatim inside `{ COMMAND; }` by silex sh, in
#            the fixture directory, with stdin </dev/null and stdout/stderr
#            to /dev/null unless COMMAND redirects them itself.
#
# Every COMMAND must be idempotent: it runs WARMUP + ITERS times in the same
# directory, and its exit status and output (stdout+stderr) on the first run
# must equal those of a run after the measured loop -- an applet that leaves
# state behind in the shell changes what the NEXT invocation does. Every applet in the applet table must have at least one `ok` and
# one `err` case; the runner refuses to start otherwise, so a new applet
# cannot be added without being covered here.
#
# Fixtures (created fresh by the runner): f (3 lines), g (sed -i target),
# d/ (dir with d/x), lnk0 -> f, mvA, sums (valid sha256sum -c list),
# badsums (mismatching list), script.sed, tmp/.

# `stdin-redirect` is not an applet: a group of its own (one silex sh
# process), run after the applets. KNOWN BUG, found by this suite: an in-process applet reads the
# shell's process-wide stdio `stdin`. A `<file` redirect dup2()s fd 0 under
# it, but nothing discards that FILE's read-ahead buffer or clears its EOF
# flag -- not before the applet runs, not before the redirect is undone. So
# the FIRST `tr a-z A-Z <f` works and every later stdin-reading applet in the
# same shell sees EOF at once (or the previous file's leftover bytes):
#     head -n1 <f5; tr a b <g2    -> tr prints the rest of f5, then g2
# The pipeline path already resets stdin (exec_pipeline: fflush+clearerr
# before dup2); simple_cmd_applet() does not. The tr/tee/xargs cases feed stdin
# through a pipeline so their applet is still exercised on every run.

# shellcheck disable=SC2034
LEAK_CASES='
basename|ok|basename /a/b/c.txt .txt
basename|err|basename
cat|ok|cat f
cat|err|cat nonexist
cat|err|cat f >/dev/full
chmod|ok|chmod 644 f
chmod|ok|chmod -R u+rwX d
chmod|err|chmod 644 nonexist
chmod|err|chmod zzz f
cp|ok|cp f f2
cp|ok|cp -rf d dcopy
cp|err|cp nonexist f3
cp|err|cp f nonexist/dir/f
cut|ok|cut -d: -f1 f
cut|err|cut -f0 f
cut|err|cut -f1 nonexist
date|ok|date -u +%Y
date|err|date -d garbage-date
dirname|ok|dirname /a/b
dirname|err|dirname
echo|ok|echo hi
echo|err|echo hi >/dev/full
env|ok|env FOO=1 true
env|err|env -u
env|err|env /nonexistent/cmd
find|ok|find d -name x
find|err|find nonexist
find|err|find d -bogus-predicate
grep|ok|grep a f
grep|ok|grep -c -E "a|b" f
grep|err|grep "\\(" f
grep|err|grep a nonexist
head|ok|head -n 2 f
head|err|head -n x f
head|err|head nonexist
install|ok|install -m 644 f inst
install|ok|install -d instdir/sub
install|err|install nonexist inst2
install|err|install -m zzz f inst3
ln|ok|ln -sf f lnk
ln|err|ln nonexist lnk2
ln|err|ln -s f nonexist/dir/lnk
mkdir|ok|mkdir -p d/sub
mkdir|err|mkdir nonexist/a/b
mkdir|err|mkdir d
mktemp|ok,volatile|mktemp -u -p tmp
mktemp|err,volatile|mktemp -p nonexist/dir
mktemp|err,volatile|mktemp -d -p nonexist/dir
mv|ok|mv mvA mvB && mv mvB mvA
mv|err|mv nonexist x
printf|ok|printf "%s %d\n" a 1
printf|err|printf "%d" abc
readlink|ok|readlink lnk0
readlink|ok|readlink -f lnk0
readlink|err|readlink f
realpath|ok|realpath f
realpath|err|realpath nonexist/dir/x
rm|ok|rm -f rmnone
rm|ok|mkdir -p rmd/x && rm -r rmd
rm|err|rm nonexist
rm|err|rm d
sed|ok|sed -n "s/a/b/p" f
sed|ok|sed 1q f
sed|ok|sed -e "\$!d" -e "y/abc/xyz/" f
sed|ok|sed -f script.sed f
sed|ok|sed -i.bak "s/a/a/" g
sed|err|sed "s/a/b/;s/x" f
sed|err|sed "/[/p" f
sed|err|sed "s/\\(a\\)/\\2/" f
sed|err|sed "b nolabel" f
sed|err|sed -e "1{" -e "p" f
sed|err|sed "p;p;k" f
sed|err|sed -f nonexist f
sed|err|sed --bogus-option
sed|err|sed p nonexist
sed|err|sed -i "s/a/b/w /nonexistent/dir/x" g
sed|err|sed -i "s/x/y/" nonexist
sed|err|sed -n "/a/w /nonexistent/dir/x" f
sh|ok|sh -c "exit 0"
sh|err|sh -c "exit 3"
sh|err|sh -c "if"
sha256sum|ok|sha256sum f
sha256sum|ok|sha256sum -c sums
sha256sum|err|sha256sum nonexist
sha256sum|err|sha256sum -c badsums
sort|ok|sort f
sort|ok|sort -o sorted f
sort|err|sort nonexist
sort|err|sort -k x f
stat|ok|stat -c %s f
stat|err|stat nonexist
tail|ok|tail -n 2 f
tail|err|tail nonexist
tail|err|tail -n x f
tee|ok|cat f | tee teeout
tee|err|cat f | tee nonexist/dir/x
touch|ok|touch f
touch|err|touch nonexist/dir/x
touch|err|touch -d garbage-date f
tr|ok|cat f | tr a-z A-Z
tr|err|tr
tr|err|cat f | tr a-z A-Z >/dev/full
wc|ok|wc -l f
wc|err|wc nonexist
xargs|ok|cat f | xargs echo
xargs|err|xargs -n x echo <f
xargs|err|cat f | xargs /nonexistent/cmd

stdin-redirect|ok,xfail|tr a-z A-Z <f
'
