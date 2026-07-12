# silex — root dispatch.
#
# This repo holds two components:
#
#   core/   the silex binary: a POSIX sh + coreutils builtins in one process
#   sdk/    the silex Docker base image (clang, mold, ninja, sccache, mimalloc)
#
# They both define `test`, `build`, `clean`, `release`, `all` and `lint`, so
# everything here is prefixed. Use `make core-<target>` or `make sdk-<target>`;
# anything the sub-Makefiles accept works.
#
#   make core-release      build the binary
#   make core-test         unit tests
#   make core-external-test  conformance suites (smoosh, modernish, ...)
#   make sdk-build         build the image from the previous silex:slim
#   make sdk-bootstrap     cold-build the image from debian:bookworm-slim
#   make sdk-test          image compat tests

.PHONY: help all test clean

help:
	@echo 'silex — two components in one repo.'
	@echo ''
	@echo '  core/  the silex binary (POSIX sh + coreutils builtins, single process)'
	@echo '  sdk/   the silex Docker base image'
	@echo ''
	@echo 'Targets are prefixed because both components define the same names:'
	@echo ''
	@echo '  make core-release        build the binary'
	@echo '  make core-test           unit tests'
	@echo '  make core-external-test  conformance suites'
	@echo '  make sdk-build           build the image'
	@echo '  make sdk-bootstrap       cold-build the image (~90-120 min)'
	@echo '  make sdk-test            image compat tests'
	@echo ''
	@echo '  make test                core-test + sdk-test'
	@echo ''
	@echo 'Any target of core/Makefile or sdk/Makefile works with its prefix.'

# Pass anything through to the right component. -C sets the working directory,
# which is why core/Makefile uses $(CURDIR) and not $(PWD) -- make does not
# update PWD under -C.
core-%:
	$(MAKE) -C core $*

sdk-%:
	$(MAKE) -C sdk $*

all: core-release

test: core-test sdk-test

clean: core-clean sdk-clean
