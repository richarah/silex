# Where the time actually goes

Measured 2026-07-13. `/bin/sh` swapped in the same image, same workload, only the
shell differs. **Every run is verified to produce a correct artifact** — see the
warning at the bottom for why that sentence is the most important one here.

## The result

| `/bin/sh` | `./configure` (zlib) | `./configure` + `make -j4` |
|---|---|---|
| dash (dynamic) | 260 ms | 1423 ms |
| busybox sh (static) | 244 ms | 1462 ms |
| **silex** | **204 ms** (22% faster) | **1362 ms** (4% faster) |

silex is **22% faster on the shell-heavy phase** and **4% faster on the whole
build**. Those two numbers are not in tension — they say the same thing from two
angles. `configure` is dominated by shell work; `configure + make` is dominated
by `gcc`. The more of your build is compilation, the less the shell matters.

**4% on a real build is a real win. It is not 2-3x.** Do not claim 2-3x.

## The mechanism: builtins avoid fork + exec + the dynamic loader

Not "no fork" — *no fork, no exec, and no dynamic loader*. That third one is the
big term, and the original README missed it.

    500 invocations of `sed` on a small file:

        dash          666 us each    forks /usr/bin/sed -- 6 shared libraries,
                                     resolved by ld.so on EVERY invocation
        busybox sh    382 us each    forks /bin/busybox -- static, so it at least
                                     skips the dynamic loader
        silex          24 us each    BUILTIN. No fork, no exec, no load.

    `type sed`:  dash -> /usr/bin/sed        silex -> a silex builtin

**27x per invocation.** A configure script runs hundreds of `sed`, `grep`, `cat`,
`rm`, `expr` and `test` calls, and that is where silex's 22% comes from.

## The old thesis was directionally right and numerically wrong

The README said:

> In a container build invoking 500 commands per minute, half the wall time is
> process creation.

and `docs/PIPE_ELIMINATION.md` priced a fork at ~500 µs. Both supporting numbers
are wrong, in opposite directions:

- **The per-command cost was undercounted.** A `fork` is ~60 µs. But forking an
  external *dynamically linked* coreutil costs ~666 µs, because `ld.so` resolves
  six libraries every single time. The fork is the cheap part.
- **The command rate was undercounted.** zlib's `./configure` alone issues
  hundreds of coreutil calls in ~250 ms. That is nowhere near "500 per minute".

So the *direction* was right — in-process builtins are the win — but the arithmetic
in the README could not be used to justify it, and anyone who checked it (as we
did) would conclude the thesis was off by two orders of magnitude. Replace the
numbers, keep the idea.

Meanwhile the subsystems built *around* that thesis were not doing anything:
`src/batch/` (io_uring) had **zero callers** and has been deleted; the fs cache's
`fscache_init()` had **zero callers** and was inert.

## Static linking is NOT the win

busybox sh is also a static multi-call binary, and it is **a wash** against dash:
6% faster on `configure`, 3% *slower* on `configure + make`. Whatever silex is
doing, "it's static" is not the explanation. The builtins are.

(An earlier revision of this document claimed static linking was worth 23%. It
was wrong. See below.)

## The interpreter is still slower than dash

    5000-iteration shell loop:   silex 37 ms   dash 10 ms   -> silex 3.7x SLOWER

silex wins on builds because builds spend their shell time *dispatching commands*,
not *interpreting control flow*. If a workload ever becomes interpretation-bound
rather than dispatch-bound, silex loses. That is the ceiling, and it is where the
optimisation effort should go next.

(It used to be 56x slower: `has_unquoted_glob()` treated `[` as a glob
metacharacter, so every `if [ ... ]` ran a full `glob()` over the working
directory. Fixed.)

## READ THIS BEFORE YOU BENCHMARK ANYTHING

**The first version of this document reported that silex was 44% faster on
`configure + make`. That number was garbage: silex's `make` was FAILING.**

It built 16 of 19 objects, died, and never produced `libz.a` — and `make` exiting
non-zero was not checked, so a crashed build was recorded as a fast one. The root
cause was a real silex bug (`$?` after `v=$(cmd)` always returned 0), which made
zlib's `./configure` conclude it was building for IBM s390x on an x86_64 machine
and emit `-DHAVE_S390X_VX -mzarch`. `configure` still exited 0.

This is the exact failure mode this repo's own `benchmarks/benchmark.sh` had —
`|| true`, with a comment cheerfully noting that "failed runs produce a
measurement of time-to-fail". A failed run is not a fast run.

**Every benchmark in this project must assert the artifact.** Not the exit code —
the artifact. Count the object files. Stat the library. If you cannot say what
the build was supposed to produce, you cannot benchmark it.

## Reproducing

```sh
make -C core release        # musl static
```

Then in a container with all three shells, for each `/bin/sh`: run
`./configure && make -j4` on zlib, time it, and **verify `libz.a` exists and
`ls *.o | wc -l` is 19**. Discard any run that does not.
