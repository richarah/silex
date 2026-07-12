# Where the time actually goes

Measured, 2026-07-12. This document exists because the project's stated thesis
was wrong, and the real mechanism is worth writing down before someone optimises
the wrong thing again.

## The thesis was wrong

The README said:

> The Unix process model forks a new process for every command. In a container
> build invoking 500 commands per minute, half the wall time is process creation.

And `docs/PIPE_ELIMINATION.md` priced a fork at ~500 µs. Those two numbers refute
each other:

    500 commands x 500 us = 0.25 s per minute = 0.4% of wall time

Not half. Off by ~125x. And 500 µs is itself generous — a real `fork()+execve()`
is 50–150 µs.

So fork elimination was never going to deliver. It didn't.

## What silex is actually good at

`/bin/sh` in a container, same image, same workload, only the shell differs:

| Workload | silex | dash | |
|---|---|---|---|
| `./configure` (zlib) | **240 ms** | 284 ms | silex 15% faster |
| `./configure` + `make -j4` | **835 ms** | 1496 ms | silex 44% faster |
| 5000-iteration shell loop | 37 ms | **10 ms** | silex **3.7x SLOWER** |

silex is *slower* at interpreting shell and *faster* at running real builds. That
is not a contradiction — it points straight at the mechanism.

## The mechanism is STARTUP, not forks

`./configure` and `make` spawn a **fresh `/bin/sh` per test and per recipe line** —
thousands of them. So what dominates is not how fast the shell runs, but how fast
it *starts*.

Best of 3, 2000 startups of `sh -c :`:

| Shell | Startup | Linkage |
|---|---|---|
| **silex** | **371 µs** | static, multi-call |
| busybox sh | 446 µs | static, multi-call |
| dash | 577 µs | dynamic |

Two separate effects, and it matters which is which:

- **dash → busybox: 577 → 446 µs (23%).** This is *purely static linking* —
  no dynamic loader, no `libc.so` resolution per exec. Any shell can have this.
- **busybox → silex: 446 → 371 µs (17%).** This is silex's own architecture:
  a smaller image, less init work, no applet table load.

silex is the fastest of the three. But roughly two thirds of its advantage over
dash is available to *any* static shell, and only a third is silex-specific.

## Consequences

**1. The image is leaving free performance on the floor.** It ships `dash` —
   dynamically linked — as `/bin/sh`, and busybox is already in the image.
   `ln -sf /bin/busybox /bin/sh` would capture most of the win today, at zero
   risk, without waiting for silex to reach POSIX conformance.

**2. Optimise startup, not forks.** The things that would actually move the
   number:
   - keep the binary small and static (page-in cost is startup cost)
   - defer anything not needed to run one command
   - avoid touching the module directory, the arena, or the intern table on a
     path that just runs `:`

   The things that would *not*:
   - `posix_spawn` instead of `fork` (measured: closes ~26 µs of a 183 µs/command
     gap; one `gcc -c` is 24 ms, i.e. ~40 commands' worth)
   - io_uring batching (was dead code; measured upside ~1 ms per 1000 unlinks)
   - the fs cache (was dead code; ~10 µs across a whole Dockerfile)

**3. The interpreter is 3.7x slower than dash and that is now the ceiling.**
   Startup is won; execution is not. If a workload ever becomes interpretation-
   bound rather than startup-bound, silex loses. See the loop benchmark above.

   (It used to be *56x* slower, because `has_unquoted_glob()` treated `[` as a
   glob metacharacter and every `if [ ... ]` ran a full `glob()` over the working
   directory. That is fixed. What remains is ordinary interpreter overhead.)

## How to reproduce

```sh
make -C core release                      # musl static
# then, in a container with both shells present:
for s in /bin/silex /bin/busybox\ sh /bin/dash; do
    t0=$(date +%s%N)
    i=0; while [ $i -lt 2000 ]; do $s -c ":" ; i=$((i+1)); done
    t1=$(date +%s%N)
    echo "$s $(( (t1-t0)/1000000 )) ms"
done
```

Do not benchmark `wc -l` against GNU `wc` and call it a container-build number.
Benchmark `docker build`.
