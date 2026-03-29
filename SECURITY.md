# matchbox Security

## Threat Model

matchbox is designed to run inside container build environments (Docker, OCI).
The primary threat is an attacker who controls:
- Container filesystem contents (Dockerfile COPY/ADD layers)
- Environment variables passed to RUN steps
- Filenames in the build context

matchbox does NOT aim to sandbox arbitrary user code. It is a build tool, not
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

## Reporting Vulnerabilities

File a GitHub issue with the label `security`. For sensitive reports, email
the maintainer directly (see git log for contact). Please include:
- Affected version/commit
- Steps to reproduce
- Expected vs actual behavior
- Impact assessment

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
