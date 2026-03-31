# silex — Unikernel Compatibility Research

**Status**: Research / Feasibility Analysis
**Last updated**: 2026-03-30

---

## Overview

A unikernel is a single-address-space OS image that packages an application and a
minimal kernel into one bootable binary. silex's goals (small binary, static link,
no runtime dependencies) make it a natural candidate for unikernel deployment as the
container build layer in a VM-based sandbox.

This document analyses compatibility with three mature unikernel frameworks and
enumerates the minimum kernel interface silex requires.

---

## Syscall Surface

The following table lists every Linux syscall class silex uses, derived from
`strace -e trace=all` on the integration test suite. Syscalls marked **required**
must be present in the unikernel; those marked **optional** are used only for
performance or optional features and can be stubbed.

| Syscall / class | Used by | Required? | Notes |
|-----------------|---------|-----------|-------|
| `read`, `write`, `close` | everything | YES | Core I/O |
| `open`, `openat`, `creat` | all file builtins | YES | |
| `fstat`, `stat`, `lstat` | find, cp, ln, stat | YES | |
| `mmap`, `munmap` | sort (O-16 fast path) | YES | Can fall back to malloc |
| `brk` / `sbrk` | malloc (glibc) | YES | |
| `fork`, `clone` | shell, pipelines | YES | Most expensive unikernel gap |
| `execve` | external commands | YES | Replaced by in-process dispatch |
| `waitpid`, `wait4` | shell | YES | Needed after fork |
| `pipe`, `pipe2` | pipelines | YES | |
| `dup`, `dup2`, `dup3` | redirects, heredocs | YES | |
| `fcntl` | redirect (F_DUPFD_CLOEXEC) | YES | |
| `lseek` | heredoc, sort | YES | |
| `getpid`, `getppid` | shell ($$ expansion) | YES | |
| `kill`, `signal`, `rt_sigaction` | trap builtin, SIGPIPE | YES | |
| `chdir`, `getcwd` | cd builtin, $PWD | YES | |
| `mkdir`, `rmdir` | mkdir, rm | YES | |
| `unlink`, `unlinkat` | rm, touch, heredoc | YES | |
| `rename`, `renameat` | mv, sed -i | YES | |
| `symlink`, `readlink` | ln, readlink | YES | |
| `chmod`, `fchmod` | chmod, install | YES | |
| `chown`, `fchown` | install (-o/-g) | optional | Requires user DB |
| `getenv` / environ | sh builtins | YES (libc) | Via libc, not direct syscall |
| `mkstemp` / `mkostemp` | sed -i, heredoc | YES (libc) | Uses open+O_EXCL internally |
| `access` | find -perm, command -v | optional | Can stub to stat |
| `opendir`, `readdir`, `closedir` | find, glob | YES | |
| `getdents64` | (via opendir) | YES | |
| `times`, `clock_gettime` | date, touch -t | optional | |
| `nanosleep` | none currently | NO | |
| `socket`, `bind`, `connect` | none currently | NO | |
| `ioctl` | isatty() | optional | Can stub → returns 0 |
| `writev` | none currently | NO | |
| `pread64` | none currently | NO | |
| `posix_fadvise` | grep, sed, sort, tail | optional | Stub to no-op |
| `io_uring_*` | batch subsystem | optional | Falls back to poll |
| `/proc/self/exe` | silex --install | optional | Can stub to empty |

**Hard requirements summary**: ~30 syscalls (POSIX file I/O, fork/exec, signals,
directory traversal). No networking. No thread synchronisation primitives beyond
`waitpid`.

---

## Framework Analysis

### 1. Unikraft (https://unikraft.org)

**Maturity**: Production-grade; POSIX compatibility layer `lib/posix-*`.

| Feature | Status |
|---------|--------|
| `fork` | Partial — `vfork` supported; full `fork` via copy-on-write needs `ukfork` (experimental in 0.17+) |
| `execve` | **Not supported** natively; in-process dispatch (silex already does this for all 27 applets) eliminates need |
| POSIX file I/O | Full (`lib/posix-fs` + `lib/vfscore`) |
| Signals | Partial (`lib/posix-signal`); `SIGPIPE`, `SIGCHLD` supported |
| `isatty` | Stubbed to 0 (non-interactive mode assumed) |
| Static binary | YES — Unikraft builds a static ELF unikernel |

**Verdict**: **Feasible with caveats.**
silex's in-process applet dispatch (which already avoids `execve` for all 27
builtins) covers the execve gap. The remaining issue is `fork` for external commands
and pipeline stages. Unikraft 0.17+ `ukfork` must be enabled. Scripts that call
only silex applets work today; scripts that invoke external binaries require fork.

**Build approach**:
```
# Clone Unikraft + silex source
git clone https://github.com/unikraft/unikraft
# Add silex as a lib (adapt Makefile to emit a .a)
# Enable: lib/posix-process, lib/posix-fs, lib/posix-signal, lib/vfscore, lib/ukfork
# Build: make menuconfig && make
```

---

### 2. OSv (https://github.com/cloudius-systems/osv)

**Maturity**: Stable; designed for running unmodified Linux apps.

| Feature | Status |
|---------|--------|
| `fork` | **Not supported** — OSv does not implement POSIX fork (single-process model) |
| `execve` | Supported via ELF loader |
| POSIX file I/O | Full (ZFS or ramfs) |
| Signals | Partial |
| Static binary | YES — OSv can boot a static PIE binary directly |

**Verdict**: **Not feasible without modification.**
OSv's explicit non-support of `fork` is a fundamental incompatibility with silex's
shell, which forks for pipelines, background jobs, and subshells. A fork-free mode
(using `posix_spawn` or thread-based simulation) would require significant shell
refactoring.

---

### 3. HermitOS / Hermit-Lite (https://github.com/hermit-os/hermit-rs)

**Maturity**: Research-oriented; Rust-first; POSIX layer incomplete.

| Feature | Status |
|---------|--------|
| `fork` | **Not supported** |
| POSIX file I/O | Partial (virtio-fs backend) |
| Signals | Minimal |
| C ABI | Supported via `hermit-libc` |
| Static binary | YES |

**Verdict**: **Not feasible** — Same fork limitation as OSv, with a less complete
POSIX layer. Suitable only for future investigation if a fork-free shell mode is
added.

---

## Minimum Kernel Interface

For silex to run on a bare-metal / unikernel target with no `fork`, the shell
must operate in **fork-free mode**. This requires:

1. **Inline pipelines**: Run pipeline stages sequentially using `dup2` + in-process
   execution rather than forking per stage. Output of each stage buffered in memory.
   (Acceptable for build scripts where pipeline data is small.)

2. **No background jobs**: `cmd &` unsupported (or runs synchronously with a warning).

3. **Subshells via function call**: `(...)` executes in-process with saved/restored
   shell state rather than fork.

With these restrictions, the minimum unikernel interface reduces to:

```
read, write, open, openat, close, fstat, stat, lstat, lseek
mmap, munmap, brk
pipe, pipe2, dup, dup2, dup3, fcntl
mkdir, rmdir, unlink, unlinkat, rename
symlink, readlink, chmod
opendir/getdents64, closedir
clock_gettime, getcwd, chdir
getpid, kill, rt_sigaction
```

That is **~25 syscalls** — achievable in any unikernel with basic POSIX support.

---

## Recommendations

| Priority | Action |
|----------|--------|
| Short-term | Test silex binary boot under Unikraft with `ukfork` enabled |
| Medium-term | Add `--fork-free` mode to shell that disables fork-dependent features |
| Long-term | Replace fork-based pipelines with in-process buffered execution |

The largest effort is the fork-free pipeline mode. Estimated ~500 lines of new code
in `exec.c` / `pipeline.c`. Not required for the initial v0.1.0 release.

---

## References

- [Unikraft POSIX support matrix](https://unikraft.org/docs/concepts/compatibility)
- [OSv design: no fork](https://github.com/cloudius-systems/osv/wiki/OSv-and-the-Lack-of-fork%282%29)
- [HermitOS syscall list](https://github.com/hermit-os/hermit-rs/blob/master/hermit/src/syscalls/mod.rs)
- [Linux syscall table](https://man7.org/linux/man-pages/man2/syscalls.2.html)
