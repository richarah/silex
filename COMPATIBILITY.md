# matchbox Compatibility Matrix

This document lists each builtin tool with its supported flags.
Columns: POSIX = POSIX-specified, GNU = GNU coreutils extension,
Core = matchbox builtin, Module = available as loadable .so,
Notes = known differences.

Legend: Y = supported, N = not supported, - = not applicable

## Shell (sh)

| Feature | POSIX | GNU/bash | matchbox | Notes |
|---------|-------|----------|----------|-------|
| -c CMD | Y | Y | Y | |
| -e (errexit) | Y | Y | Y | |
| -u (nounset) | Y | Y | Y | |
| -x (xtrace) | Y | Y | Y | |
| -n (noexec) | Y | Y | Y | |
| -f (noglob) | Y | Y | Y | |
| -o pipefail | N | Y | Y | bash extension |
| Pipes | Y | Y | Y | |
| Redirects > >> < << <<- | Y | Y | Y | |
| 2>&1, &> | Y | Y | Y | |
| Here-docs | Y | Y | Y | |
| $() command substitution | Y | Y | Y | |
| $(( )) arithmetic | Y | Y | Y | |
| ${var:-}, ${var:+}, etc. | Y | Y | Y | All POSIX forms |
| ${var/pat/repl} | N | Y | Y | bash extension |
| Globbing *, ?, [...] | Y | Y | Y | |
| if/elif/else/fi | Y | Y | Y | |
| while, until | Y | Y | Y | |
| for..in | Y | Y | Y | |
| case..esac | Y | Y | Y | |
| Functions | Y | Y | Y | |
| local | N | Y | Y | common extension |
| trap | Y | Y | Y | |
| Background & | Y | Y | Y | |

## cp

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -r/-R | Y | Y | Y | - | |
| -p | Y | Y | Y | - | |
| -f | Y | Y | Y | - | |
| -i | Y | Y | Y | - | |
| -v | N | Y | Y | - | |
| -n | N | Y | Y | - | |
| -u | N | Y | Y | - | |
| -T | N | Y | Y | - | |
| -L/-H/-P | Y | Y | Y | - | |
| -a | N | Y | Y | - | |
| --reflink | N | Y | N | Y | cp_reflink.so |
| --sparse | N | Y | N | N | not implemented |

## echo

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -n | Y | Y | Y | - | |
| -e | N | Y | Y | - | |
| -E | N | Y | Y | - | |

## mkdir

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -p | Y | Y | Y | - | |
| -v | N | Y | Y | - | |
| -m MODE | Y | Y | Y | - | |

## cat

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -n | N | Y | Y | - | |
| -b | N | Y | Y | - | |
| -s | N | Y | Y | - | |
| -v | N | Y | Y | - | |
| -e/-t/-A | N | Y | Y | - | |
| -u | Y | Y | Y | - | no-op per POSIX |

## chmod

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| octal mode | Y | Y | Y | - | |
| symbolic mode | Y | Y | Y | - | |
| -R | Y | Y | Y | - | |
| -v | N | Y | Y | - | |
| --reference | N | Y | Y | - | |

## mv

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -f | Y | Y | Y | - | |
| -i | Y | Y | Y | - | |
| -v | N | Y | Y | - | |
| -n | N | Y | Y | - | |
| -u | N | Y | Y | - | |
| -T | N | Y | Y | - | |
| cross-fs | Y | Y | Y | - | copy+unlink fallback |

## rm

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -r/-R | Y | Y | Y | - | |
| -f | Y | Y | Y | - | |
| -i | Y | Y | Y | - | |
| -v | N | Y | Y | - | |
| --no-preserve-root | N | Y | N | - | REJECTED for safety |
| rm -rf / | - | dangerous | N | - | always rejected |

## ln

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -s | Y | Y | Y | - | |
| -f | Y | Y | Y | - | |
| -v | N | Y | Y | - | |
| -n | N | Y | Y | - | |
| -r | N | Y | Y | - | |
| -T | N | Y | Y | - | |

## touch

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -a | Y | Y | Y | - | |
| -m | Y | Y | Y | - | |
| -c | Y | Y | Y | - | |
| -t STAMP | Y | Y | Y | - | |
| -r REF | N | Y | Y | - | |
| -d DATE | N | Y | Y | - | simple subset |

## head

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -n N | Y | Y | Y | - | |
| -n -N | N | Y | Y | - | all-but-last-N |
| -c N | N | Y | Y | - | |
| -q/-v | N | Y | Y | - | |

## tail

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -n N | Y | Y | Y | - | |
| -n +N | Y | Y | Y | - | |
| -c N | N | Y | Y | - | |
| -f | N | Y | Y | - | inotify on Linux |
| --pid | N | Y | Y | - | |
| -q/-v | N | Y | Y | - | |

## wc

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -c | Y | Y | Y | - | |
| -l | Y | Y | Y | - | |
| -w | Y | Y | Y | - | |
| -m | Y | Y | Y | - | |
| -L | N | Y | Y | - | |

## sort

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -b/-d/-f/-i/-n/-r/-u | Y | Y | Y | - | |
| -o FILE | Y | Y | Y | - | |
| -t SEP | Y | Y | Y | - | |
| -k KEYDEF | Y | Y | Y | - | |
| -s (stable) | N | Y | Y | - | |
| -z | N | Y | Y | - | |
| -g/-h/-V | N | Y | Y | - | |

## grep

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -E/-F | Y | Y | Y | - | |
| -c/-i/-l/-n/-q/-s/-v | Y | Y | Y | - | |
| -e/-f | Y | Y | Y | - | |
| -r/-R | N | Y | Y | - | |
| -w | N | Y | Y | - | |
| --include/--exclude | N | Y | Y | - | |
| --color | N | Y | Y | - | |
| -P (PCRE) | N | Y | N | Y | grep_pcre.so |

## sed

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -n | Y | Y | Y | - | |
| -e/-f | Y | Y | Y | - | |
| -E/-r (ERE) | Y | Y | Y | - | |
| -i (in-place) | N | Y | Y | Y | sed_inplace.so for backup suffix |
| s,d,p,q,y,a,i,c,= | Y | Y | Y | - | |
| h,H,g,G,x (hold space) | Y | Y | Y | - | |
| b,t,: (labels) | Y | Y | Y | - | |
| n,N (next) | Y | Y | Y | - | |
| Non-/ delimiters | N | Y | Y | - | |

## find

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -name/-iname | Y | Y | Y | - | |
| -type | Y | Y | Y | - | |
| -exec...; / -exec...+ | Y | Y | Y | - | |
| -print/-print0 | Y | Y | Y | - | |
| -maxdepth/-mindepth | N | Y | Y | - | |
| -delete/-empty/-newer | N | Y | Y | - | |
| -mtime/-atime/-size | Y | Y | Y | - | |
| -perm/-user/-group | Y | Y | Y | - | |
| -and/-or/-not | Y | Y | Y | - | |
| -L | Y | Y | Y | - | |

## xargs

| Flag | POSIX | GNU | matchbox | Module | Notes |
|------|-------|-----|----------|--------|-------|
| -0 | N | Y | Y | - | |
| -I REPL | Y | Y | Y | - | |
| -n N | Y | Y | Y | - | |
| -P N | N | Y | Y | - | |
| -r | N | Y | Y | - | |
| -d DELIM | N | Y | Y | - | |

## basename/dirname/readlink/stat/date/printf/install/tr/cut

All POSIX-specified flags are supported. Common GNU extensions are
supported. Unusual GNU flags fall through to the external tool via PATH.

See source files in src/core/ for full flag details.
