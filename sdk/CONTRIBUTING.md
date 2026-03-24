# Contributing

## Build and test

```bash
make verify-sources    # verify SHA256 values in sources.json
make bootstrap         # cold build from debian:bookworm-slim (~90-120 min)
make build             # self-hosted build, reuses previous silex:slim (~15-20 min)
make test              # unit + compat tests
```

## Code rules

- No Python in image internals. Scripts use sh/awk/sed/grep. C for binaries.
- POSIX sh only. No `[[`, no arrays, no bashisms. Scripts run under dash.
- Pin every version. No `latest`, no floating tags, no unpinned `apk add`.
- Verify SHA256 for every source tarball. Add to `sources.json`.
- Every file in the final image justifies its presence.

## Adding package mappings

Edit `config/package-mapping.json`:

```json
"debian-package-name": "wolfi-package-name"
```

Map to `""` if there is no Wolfi equivalent (package is silently skipped).
Test with `docker run --rm silex:slim apt-get install -y <package>`.

## Adding a tool

1. Add source tarball to `sources.json` with version and SHA256.
2. Add build step to `dockerfiles/Dockerfile.bootstrap`.
3. COPY binary into the final stage.
4. Add a test in `scripts/test.sh`.
5. Update the tools table in README.md.

## Pull requests

- One concern per PR.
- `make test` must pass.
- `silex doctor` must exit 0 in the built image.
- No new documentation files. README.md, CONTRIBUTING.md, docs/LICENSING.md only.
