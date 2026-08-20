# silex flag gap analysis

Generated: 2026-03-31. **Partially re-measured 2026-08-18 — see "What has
landed since" below before trusting any individual entry.**

This document classifies every GNU flag that silex builtins don't currently handle.

## What has landed since the March triage

The classification below is a triage of GNU's flag surface as it stood on
2026-03-31. A good deal of it has been implemented since, so an entry appearing
here does **not** mean the flag is missing. The lists were re-probed against the
current binary on 2026-08-18 for the five tools with the largest sections; the
rest have not been re-checked and should be treated as unverified.

**grep** — 11 of the 15 BUILTIN entries are implemented: `-o -m -A -B -C -H -h
-L -Z -b -x`. Still absent: `--line-buffered`, `--label=LABEL`, `-z`,
`--exclude-dir=DIR`.

**sort** — implemented: `-M`, `-R`, `-c`. Still absent: `-C`, `-T DIR`,
`--files0-from=FILE`. Note that `-V`/`--version-sort` and
`-h`/`--human-numeric-sort` are listed below as MODULE work; both are now
builtins, and `sort_version.so` / `sort_human.so` were never needed.

**xargs** — implemented: `-a`, `-L`, `-s`, `-t`. Still absent: `-x`,
`--show-limits`, `-E STRING`. (`-p`, prompt, remains deliberately unimplemented:
it is useless without a terminal.)

**install** — implemented: `-D`, `-m`. Still absent: `-b` (backup), `-C`
(compare).

**find** — `-print0`, `-newer` and `-size` are implemented; these were the
critical-path entries.

The authoritative, re-runnable figure is
`bash tests/conformance/new_flags_test.sh build/bin/silex` (28/28 as of
2026-08-18), and per-tool status is in
[IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md). Prefer either to the lists
below when they disagree: the lists are a plan, and only the tests are evidence.

## Classification

- **BUILTIN**: Add to builtin (common, <100 lines, no external libs)
- **MODULE**: Implement as module (complex, library-dependent, or rarely used)
- **PASSTHROUGH**: Accept silently (no-op in containers, but must not error)
- **UNSUPPORTED**: Document reason (interactive-only, GUI, or unused)

## grep

**BUILTIN** (add ~470 lines):
- `-o` - print only matching portion
- `-m N` - stop after N matches
- `-A N` - after-context lines
- `-B N` - before-context lines
- `-C N` - combined context
- `-H` - always print filename
- `-h` - never print filename
- `-L` - files without match
- `-Z` - null-terminated filenames
- `-b` - byte offset
- `--line-buffered` - force line buffering
- `--label=LABEL` - stdin label
- `--exclude-dir=DIR` - skip directories
- `-x` - match whole line
- `-z` - null-terminated input

**MODULE** (grep_pcre.so):
- `-P` / `--perl-regexp` - PCRE2

**PASSTHROUGH**:
- `--binary-files=TYPE`
- `-d ACTION` / `--directories`
- `-D ACTION` / `--devices`

## sort

**BUILTIN** (add ~300 lines):
- `-M` - month sort
- `-R` - random sort
- `-c` / `-C` - check sorted
- `-T DIR` - temp directory
- `--files0-from=FILE`

**MODULE**:
- `-V` / `--version-sort` → sort_version.so
- `-h` / `--human-numeric-sort` → sort_human.so
- `--parallel=N` → sort_parallel.so

**PASSTHROUGH**:
- `--batch-size=N`
- `--buffer-size=SIZE`
- `--compress-program=CMD`

## find

**BUILTIN** (add ~1550 lines - LARGEST):
- `-print0` - **CRITICAL** for xargs -0
- `-printf FORMAT` - formatted output
- `-exec {} +` - batch execution
- `-execdir {} \;` / `-execdir {} +`
- `-newer FILE` - modification time comparison
- `-perm MODE` - permission matching
- `-user NAME/UID` / `-group NAME/GID`
- `-size N[cwbkMG]`
- `-mtime N` / `-atime N` / `-ctime N` / `-mmin N` / `-amin N`
- `-newerXY REF`
- `-prune` - don't descend
- `-not` / `!`, `-and` / `-a`, `-or` / `-o` - logical operators
- `\( EXPR \)` - grouping
- `-depth` - depth-first
- `-xdev` - don't cross filesystems
- `-samefile FILE` / `-links N` / `-inum N`
- `-readable` / `-writable` / `-executable`
- `-regex PATTERN` / `-iregex PATTERN`
- `-path PATTERN` / `-ipath PATTERN`
- `-follow` / `-L` / `-H` / `-P`

## sed

**BUILTIN** (add ~720 lines):
- `-f FILE` - script from file
- `a\TEXT` / `i\TEXT` / `c\TEXT` - text insertion
- `r FILE` / `w FILE` - file I/O
- `=` - line number
- `l` - list unambiguous
- Address ranges: `10,20`, `/start/,/end/`, `ADDR!`, `0~N`
- `N` / `D` / `P` - multi-line pattern space

**MODULE** (sed_extended.so):
- `h` / `H` / `g` / `G` / `x` - hold space
- `b` / `t` / `:label` - branching

**PASSTHROUGH**:
- `--debug`
- `--posix`
- `--sandbox`
- `-u` / `--unbuffered`
- `-l` / `--line-length`

## xargs

**BUILTIN** (add ~300 lines):
- `-a FILE` - read args from file
- `-L N` - max lines per invocation
- `-s N` - max command length
- `-t` - trace commands
- `-p` - prompt (warn: not useful in containers)
- `-x` - exit if too long
- `--process-slot-var=VAR`
- `--show-limits`
- `-E STRING` - end-of-file marker

## install

**BUILTIN** (add ~275 lines):
- `-D` - **CRITICAL**: create leading directories
- `-t DIR` - target directory
- `-T` - treat dest as file
- `-c` - (passthrough: POSIX compat)
- `-s` - strip binaries (fork to strip)
- `-p` - preserve timestamps
- `-v` - verbose
- `-b` / `--backup` / `--suffix` - backup support

## cp

**BUILTIN** (add ~200 lines):
- `--preserve=ATTR_LIST` / `--no-preserve`
- `--parents` - recreate source path
- `-t DIR` - target directory
- `-T` - no target directory
- `-l` - hardlink instead of copy
- `-s` - symlink instead of copy
- `-d` - same as --no-dereference --preserve=links
- `--attributes-only`
- `--remove-destination`
- `--strip-trailing-slashes`

**MODULE**:
- `--reflink` → cp_reflink.so (EXISTS)
- `--sparse` → cp_sparse.so
- `--backup` → cp_backup.so
- `--progress` → cp_progress.so (silex extension)

## Other builtins summary

**cat** (add ~60 lines): `-A`, `-E`, `-T` and long aliases
**head** (add ~100 lines): `--bytes` with suffixes, `-v`, `-z`
**tail** (add ~150 lines): `--bytes`, `-v`, `-z`, `--retry`, `-F`
**wc** (add ~80 lines): `--files0-from`, `-L`, `--total`
**touch** (add ~50 lines): `--no-create`, `--date`, `--no-dereference`
**chmod** (add ~80 lines): `--changes`, `--quiet`, `--reference`
**ln** (add ~100 lines): `--backup`, `--no-dereference`, `--physical`, `--logical`
**mkdir** (add ~10 lines): `--context` (SELinux, passthrough)
**rm** (add ~100 lines): `-d`, `-I`, `--one-file-system`, `--preserve-root`
**mv** (add ~120 lines): `--backup`, `-t`, `-T`, `--strip-trailing-slashes`, `-v`
**tr** (add ~150 lines): `-c`, character classes, octal/hex escapes
**cut** (add ~120 lines): `-b`, `-c`, `--complement`, `-s`, `--output-delimiter`, `-z`
**date** (add ~80 lines): `-u`, `-I`, `-R`, `-r` (rest to date_gnu module)
**sha256sum** (add ~200 lines): `-c`, `--ignore-missing`, `--quiet`, `--status`, `--strict`, `-z`
**env** (add ~100 lines): `-i`, `-u`, `-0`, `-C`, `-S`
**stat** (add ~150 lines): `-L`, `-f`, `-t`, basic `-c` directives (complex to stat_format module)

## Summary

**Total new builtin code: ~5800 lines**
**Total new modules: 18 modules, ~5400 lines**

**Critical path items** (must do first):
1. find `-print0` (for xargs -0 pipeline)
2. grep `-A/-B/-C` (context, heavily used)
3. install `-D` (Makefiles depend on this)
4. xargs `-L` (common in scripts)
5. sed `-f` (script files)

**Module dispatch prerequisites:**
- Wire `registry_lookup()` into all core tools
- Check flag, if unknown, try module before error

Done for `grep`, `sort`, `xargs`, `cp` and `install` (2026-08-18 for the last
two: they had the lookup only in their SHORT-flag loop, so a long option was
looked up under the name `"--"` and `cp --reflink` could never reach
`modules/cp_reflink.so`). `find` has the lookup but cannot use a hit: it
evaluates a predicate TREE, and the module API has no way to inject a node into
one. Extending the API for that is unscheduled.
