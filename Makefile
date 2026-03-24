# Silex v2 build system.
# Targets: verify-sources, bootstrap, build, release, cache, test
#
# Typical workflow:
#   make verify-sources   (once, to populate SHA256 values)
#   make bootstrap        (slow: ~90-120 min cold start)
#   make build            (fast: ~15-20 min, requires silex:slim in registry)
#   make release          (PGO + BOLT, runs after build)
#   make cache            (companion cache image)
#   make test             (compat + integration tests)

VERSION    ?= 2.0.0
REGISTRY   ?= localhost
IMAGE      ?= silex
TAG        ?= slim
FULL_TAG    = $(REGISTRY)/$(IMAGE):$(TAG)

DOCKER     ?= docker
DOCKER_FLAGS ?= --progress=plain

.PHONY: all verify-sources bootstrap build release cache test clean

all: build

# ---------------------------------------------------------------------------
# verify-sources: download all source tarballs and print SHA256.
# Review output, then update sources.json and Dockerfile.bootstrap ARG values.
# ---------------------------------------------------------------------------
verify-sources:
	@sh scripts/verify-sources.sh

# ---------------------------------------------------------------------------
# bootstrap: cold-start build from debian:bookworm-slim.
# No previous Silex image required. Slow (~90-120 min).
# ---------------------------------------------------------------------------
bootstrap:
	$(DOCKER) build $(DOCKER_FLAGS) \
	    -f dockerfiles/Dockerfile.bootstrap \
	    -t $(FULL_TAG) \
	    --build-arg TARGETARCH=$(shell uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/') \
	    .

# ---------------------------------------------------------------------------
# build: self-hosted build using previous silex:slim.
# Produces identical output to bootstrap. ~15-20 min with warm sccache.
# ---------------------------------------------------------------------------
build:
	$(DOCKER) build $(DOCKER_FLAGS) \
	    -f dockerfiles/Dockerfile \
	    -t $(FULL_TAG) \
	    --build-arg TARGETARCH=$(shell uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/') \
	    .

# ---------------------------------------------------------------------------
# release: PGO + BOLT pass. Runs after build.
# Rebuilds sccache and zstd with profile data, then runs BOLT.
# ---------------------------------------------------------------------------
release:
	$(DOCKER) build $(DOCKER_FLAGS) \
	    -f dockerfiles/Dockerfile \
	    -t $(FULL_TAG) \
	    --build-arg SILEX_PGO=1 \
	    --build-arg TARGETARCH=$(shell uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/') \
	    .

# ---------------------------------------------------------------------------
# cache: build companion silex-cache:VERSION image.
# Run after silex:slim is built. Snapshot current APKINDEX + pre-fetch packages.
# ---------------------------------------------------------------------------
cache:
	@sh build-cache.sh $(VERSION)

# ---------------------------------------------------------------------------
# test: run compat tests (7 Dockerfiles) + integration tests.
# Requires silex:slim to be built.
# ---------------------------------------------------------------------------
test:
	@sh tests/compat/run-compat.sh
	@echo "compat: done"

test-integration:
	@sh scripts/test.sh

# ---------------------------------------------------------------------------
# lint: check Dockerfiles for silex lint rules.
# ---------------------------------------------------------------------------
lint:
	$(DOCKER) run --rm silex:slim silex lint dockerfiles/Dockerfile.bootstrap
	$(DOCKER) run --rm silex:slim silex lint dockerfiles/Dockerfile

# ---------------------------------------------------------------------------
# clean: remove all silex images from local Docker daemon.
# ---------------------------------------------------------------------------
clean:
	-$(DOCKER) rmi $(FULL_TAG) 2>/dev/null
	-$(DOCKER) rmi silex-cache:$(VERSION) 2>/dev/null
