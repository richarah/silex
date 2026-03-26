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

## Adding packages

silex uses silex-packages, a repository of Debian packages repacked as APKs.
To add a package, add it to `config/seeds.list` in the silex-packages repo.
Test with `docker run --rm silex:slim apt-get install -y <package>`.

## Adding a tool

1. Add source tarball to `sources.json` with version and SHA256.
2. Add build step to `dockerfiles/Dockerfile.bootstrap`.
3. COPY binary into the final stage.
4. Add a test in `scripts/test.sh`.
5. Update the tools table in README.md.

## Releasing to ghcr.io

The release workflow triggers on any `v*` tag and pushes
`ghcr.io/richarah/silex:slim` to GitHub Container Registry.

```sh
git tag v2.0.0
git push origin master v2.0.0
```

The workflow runs `make bootstrap` (cold build from debian:bookworm-slim).
First run takes ~90-120 min. After the first image is published, CI
switches to `make build` (self-hosted, ~15-20 min).

Images pushed:

```
ghcr.io/richarah/silex:slim
ghcr.io/richarah/silex:slim-<version>
ghcr.io/richarah/silex:slim-latest
```

The silex-packages CI uses `ghcr.io/richarah/silex:slim` as its build
container. Publish silex:slim before triggering a silex-packages build.

The `keys/` directory contains the silex-packages public signing key
(`*.rsa.pub`). It is committed and COPY'd into the image at
`/etc/apk/keys/`. The matching private key lives only in the
silex-packages repo secrets (`SILEX_PKG_RSA`). Never put a private
key in this repo.

## Pull requests

- One concern per PR.
- `make test` must pass.
- `silex doctor` must exit 0 in the built image.
- No new documentation files. README.md, CONTRIBUTING.md, docs/LICENSING.md only.
