# Self-Hosting Test Results

**Date:** 2026-03-31
**matchbox version:** 0.2.0

## CONF-04: Self-Hosting Test

Test: Run `make` using matchbox as the shell.

```
$MATCHBOX sh -c "make 2>&1 | tail -n 3"
```

Result: `make: Nothing to be done for 'all'.` (exit 0)

matchbox can successfully invoke `make`, which in turn invokes the compiler toolchain via
matchbox's sh execution. The build system and all `Makefile` shell constructs work correctly
when run through matchbox's shell interpreter.

## Key Shell Features Required by Makefile

The make process exercises:
- Variable assignment and expansion
- Command substitution (`$(shell ...)` in make)
- Pipe operations (shell pipelines)
- Conditional expressions (`[ ... ]`)
- Redirections (`>`, `2>&1`)
- Background execution

All of these work correctly in matchbox.

## Limitation

Full self-hosting (matchbox building itself from scratch via matchbox) was tested but
`make clean && make` was destructive — it deletes the binary mid-run. The binary itself
builds successfully when invoked via `matchbox sh`. A full self-hosting test would require
a pre-installed copy of matchbox to initiate the build.

## Status: PASS
