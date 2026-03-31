# silex Security

## Threat Model

silex is designed to run inside container build environments (Docker, OCI).
The primary threat is an attacker who controls:
- Container filesystem contents (Dockerfile COPY/ADD layers)
- Environment variables passed to RUN steps
- Filenames in the build context

silex does NOT aim to sandbox arbitrary user code. It is a build tool, not
a security boundary. However, it avoids a number of dangerous patterns common
in ad hoc shell scripts.

## Input Handling

All user-provided strings are stored in `strbuf_t` (src/util/strbuf.h), a
bounds-checked heap-managed buffer that prevents buffer overflows. Direct
`char[]` arrays with user-controlled content are prohibited by convention.

All filesystem paths are normalized or canonicalized before operations:
- `path_normalize()`: lexical resolution of `.` and `..` (no syscall)
- `path_canon()`: full `realpath(3)` canonicalization (resolves symlinks)

## rm Safety

`rm -rf /` is always rejected. Detection:
1. Strip trailing slashes from the path.
2. If the result is empty or equals `/`, reject with error and exit 1.
3. Canonicalize with `path_canon()` and check again.

`rm --no-preserve-root` is rejected unconditionally. There is no way to
override the `/` protection.

## Module Loading Security

Every `dlopen()` call in `src/module/loader.c` performs these checks before
opening the .so:

1. `lstat()` the .so path -- if it fails, reject.
2. If `S_ISLNK(st.st_mode)`: reject ("module path is a symlink").
3. `stat()` the .so path -- if `dev`/`ino` differ from `lstat`, reject
   (symlink race detection).
4. `st.st_uid` must be 0 or `getuid()`: reject otherwise ("wrong owner").
5. `(st.st_mode & S_IWOTH) == 0`: reject if world-writable.
6. `lstat()` the containing directory: same ownership and world-writable
   checks on the directory.
7. No group-write if group != `getgid()`.

No caching of security check results. Checks run on every `dlopen()`.

## Code Safety Invariants

These are enforced by convention and verified by code review:

- No `alloca()` with user-controlled sizes
- No `system()` or `popen()` in any builtin
- Every `snprintf()` return checked for truncation
- Every `malloc()`/`realloc()` return checked for NULL
- No `gets()`, `sprintf()`, `strcpy()`, `strcat()`
- Temporary files via `mkstemp()` only; never `tmpnam()`/`mktemp()`

Verified by:
```
grep -rn "alloca\|system(\|popen(" src/
grep -rn "gets\|sprintf\|strcpy\|strcat" src/
```

## Build Security

The Makefile uses:
- `-Wall -Wextra -Werror -pedantic`: all warnings are errors
- `-fsanitize=address,undefined` in debug builds
- Static linking: no LD_PRELOAD attack surface

To enable stack protection and ASLR (recommended for distribution builds):
```
CFLAGS_EXTRA = -fstack-protector-strong -fpie
LDFLAGS_EXTRA = -pie
```
These are not enabled by default because they conflict with `-static` linking.

## Resource Exhaustion Caps

silex enforces hard limits to prevent resource exhaustion from malicious or
pathological inputs:

| Resource | Limit | Location |
|----------|-------|----------|
| Shell function recursion depth | 1000 calls | `SHELL_MAX_CALL_DEPTH` in `shell.h` |
| PATH lookup cache entries | 256 buckets (chained) | `sh->path_cache[256]` in `shell.h` |
| Arena block size | 4096 bytes default | `ARENA_BLOCK_SIZE` in `arena.h` |
| Pipeline stages | 255 | `exec_pipeline()` in `exec.c` |

When the recursion limit is reached, the shell prints an error and returns exit
code 1. No crash or stack overflow occurs.

The PATH lookup cache is bounded by the number of distinct command names in the
script (at most 256 buckets, chained). Cache entries are freed in `shell_free()`.

## Signal Handling

The shell ignores `SIGPIPE` (set to `SIG_IGN` in `shell_init`). Each child
process restores `SIGPIPE` to `SIG_DFL` before `execv`. This matches the
behaviour of bash and dash and prevents silent truncation of pipeline output.

### trap builtin

User-defined signal traps (`trap CMD SIGNAL`) install a signal handler that
calls `shell_run_string()` from within the handler context. This is a
deliberate design choice for POSIX compliance but imposes constraints:

- Signal handlers use `shell_run_string()` with the same parse arena as the
  main script. Re-entrant arena allocation is safe because the arena is a
  bump allocator — it only advances, never frees mid-execution.
- `waitpid()` calls in the main execution loop retry on `EINTR` so that
  signals delivered during child-process waits do not corrupt exit status.
- The EXIT trap (`trap CMD EXIT`) fires in two places:
  1. `exec_builtin_exit()` — before calling `exit(code)` for explicit `exit`
  2. `applet_sh()` — after the main script completes, before `shell_free()`
  The action is cleared before execution to prevent infinite re-entry.
- `SIGPIPE` in child processes is always reset to `SIG_DFL`, regardless of
  any parent trap for SIGPIPE.

## Reporting Vulnerabilities

File a GitHub issue with the label `security`. For sensitive reports, email
the maintainer directly (see git log for contact). Please include:
- Affected version/commit
- Steps to reproduce
- Expected vs actual behavior
- Impact assessment

## Static vs Dynamic Build Security

The musl static build (`make release`) and the glibc dynamic build (`make release-glibc`)
have different security profiles:

| Property | musl static | glibc dynamic |
|----------|------------|---------------|
| LD_PRELOAD attack surface | None (static binary) | Present (same as any dynamically linked binary) |
| ASLR | PIE: yes (-static-pie) | PIE: yes (-pie) |
| Full RELRO | Yes (--gc-sections removes unneeded GOT entries) | Yes |
| BIND_NOW | Yes (-z now) | Yes |
| Non-executable stack | Yes (-z noexecstack) | Yes |
| Stripped | Yes (make release strips -s) | Yes (make release-glibc strips -s) |
| Dependency on host libc | None | Requires matching glibc version |

For container images, the musl static build is preferred: it is fully self-contained,
immune to LD_PRELOAD injection, and portable across any Linux kernel >= 3.17.

The glibc build is suitable for development and for container base images that already
include glibc (Debian, Ubuntu, RHEL, Alpine with glibc compatibility layer).

Module libc tagging (`silex_module_t.libc`) prevents loading a musl-compiled .so
into a glibc build and vice versa, avoiding subtle ABI mismatches.

## Privilege Handling

silex does not call `setuid()`, `setgid()`, `seteuid()`, or `setegid()`. It does
not drop or acquire privileges at runtime.

Specific tools that interact with ownership:

- **install -o USER / -g GROUP**: calls `chown(2)` on the installed file. If the
  process does not have `CAP_CHOWN`, the call fails and install returns exit code 1
  with an error message. No silent skip.
- **chmod**: calls `chmod(2)`. Fails with exit code 1 if the process lacks write
  permission on the file. No silent skip.
- **cp -p / mv**: preserves mode and timestamps via `chmod(2)` and `utimes(2)`.
  Ownership preservation (`-p` chown) requires `CAP_CHOWN`; failure is non-fatal
  (cp continues but reports the error).
- **rm**: calls `unlink(2)` / `rmdir(2)`. Fails with exit code 1 if the process
  lacks write permission on the containing directory.

Recommendation: run silex as the same user as the container build process (typically
root inside a container, or a dedicated build user). Do not run silex as a more
privileged user than the build requires.

## Environment Variable Handling

silex reads the following environment variables:

| Variable | Purpose | Security note |
|----------|---------|---------------|
| PATH | Command resolution with PATH lookup cache | Each directory is validated via `stat()` per lookup; no TOCTOU |
| HOME | Tilde expansion (~) | Accepted as provided; no validation |
| IFS | Word splitting in the shell | A hostile IFS affects expansion; this is expected POSIX behaviour |
| PWD | Working directory hint | Overridden by `getcwd()` if inconsistent |
| SILEX_MODULE_PATH | Extra module search directory | Validated with same security checks as default module dir |
| SILEX_FSCACHE_TTL | Filesystem cache TTL in seconds | Validated as a non-negative integer; invalid values use the default |
| SILEX_TRACE | Shell tracing level (1 or 2) | Informational only; no privilege escalation; output goes to stderr |
| SILEX_FORCE_FALLBACKS | Disable io_uring/inotify (set to any value) | For testing only; disables performance features, not security controls |

**LD_PRELOAD**: mitigated completely in the musl static build (static binaries ignore
LD_PRELOAD). The glibc dynamic build is susceptible to LD_PRELOAD injection, which is
the same risk as any other dynamically linked binary running inside the container.
This is not a silex-specific vulnerability; it is a property of dynamic linking.

**IFS safety**: the shell does not apply IFS splitting to literal (unquoted) command
words; IFS only applies when `$variable` is expanded without quotes. A hostile
`IFS='/'` could affect path splitting in scripts that use unquoted variable expansions.
This matches the behaviour of dash, bash, and all POSIX shells.

**PATH safety**: the PATH lookup cache (`path_cache[256]`, FNV-1a hashed) stores
resolved binary paths. Cache entries are invalidated when the directory's mtime
changes. Entries are verified with `stat()` at lookup time before `execv()`.

silex is not a security boundary. It runs inside an already-sandboxed container
build environment where the container runtime (Docker, containerd, etc.) provides
the primary isolation.

## Build Reproducibility

The `release` and `release-glibc` targets produce reproducible output for identical
source trees and compilers when `SOURCE_DATE_EPOCH` is set:

```sh
make release SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
```

Properties:

- No `__DATE__` or `__TIME__` macros are used in `src/` (verified: `grep -r '__DATE__\|__TIME__' src/` returns no matches).
- The `SILEX_BUILD_DATE` macro is only defined when `SOURCE_DATE_EPOCH` is set,
  so builds without it are not affected.
- `--build-id=sha1` produces a content-based build ID: identical input produces
  identical output on the same compiler.
- Object files are deterministic: no embedded timestamps.

To verify reproducibility:
```sh
B1=$(make release SOURCE_DATE_EPOCH=1700000000 2>/dev/null && sha256sum build/bin/silex)
make clean
B2=$(make release SOURCE_DATE_EPOCH=1700000000 2>/dev/null && sha256sum build/bin/silex)
[ "$B1" = "$B2" ] && echo "Reproducible" || echo "NOT reproducible"
```

## Audit Checklist

Manual review items (run before each release):

- [ ] All path arguments canonicalized before filesystem ops
- [ ] No string literals compared with `==` (use strcmp)
- [ ] All regex patterns freed after use
- [ ] No format string vulnerabilities (user-controlled strings never used as
      format arguments)
- [ ] Module loader security checks present and correct
- [ ] `rm -rf /` rejection tested
- [ ] No world-writable module directory accepted
- [ ] No symlinks in module paths accepted
