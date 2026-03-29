# Makefile -- matchbox container build runtime
# Builds a static binary; musl-gcc used if available, otherwise gcc with -static.

# --- Compiler selection -------------------------------------------------------
MUSL_GCC := $(shell command -v musl-gcc 2>/dev/null)
CLANG     := $(shell command -v clang 2>/dev/null)

ifdef MUSL_GCC
    CC = musl-gcc
else ifdef CLANG
    CC = clang
else
    CC = gcc
endif

# --- Flags --------------------------------------------------------------------
CFLAGS_COMMON  = -std=c11 -Wall -Wextra -Werror -pedantic \
                 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                 -fstack-protector-strong
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -flto -fPIE
CFLAGS_DEBUG   = $(CFLAGS_COMMON) -O0 -g -fsanitize=address,undefined

CFLAGS ?= $(CFLAGS_RELEASE)

# Static linking: musl-gcc handles this automatically; for glibc we use -static-pie
# (-static-pie requires glibc >= 2.16, present on this machine with 2.42)
ifdef MUSL_GCC
    LDFLAGS_STATIC =
else
    LDFLAGS_STATIC = -static-pie
endif

LDFLAGS ?= $(LDFLAGS_STATIC)

# Link with dl for module loading (dlopen)
LDLIBS = -ldl

# --- Directories --------------------------------------------------------------
SRCDIR   = src
OBJDIR   = build/obj
BINDIR   = build/bin

# --- Sources ------------------------------------------------------------------
UTIL_SRCS = \
    $(SRCDIR)/util/strbuf.c \
    $(SRCDIR)/util/path.c \
    $(SRCDIR)/util/error.c \
    $(SRCDIR)/util/arena.c \
    $(SRCDIR)/util/platform.c

CORE_SRCS = \
    $(SRCDIR)/core/echo.c \
    $(SRCDIR)/core/mkdir.c \
    $(SRCDIR)/core/cp.c \
    $(SRCDIR)/core/cat.c \
    $(SRCDIR)/core/chmod.c \
    $(SRCDIR)/core/mv.c \
    $(SRCDIR)/core/rm.c \
    $(SRCDIR)/core/ln.c \
    $(SRCDIR)/core/touch.c \
    $(SRCDIR)/core/head.c \
    $(SRCDIR)/core/tail.c \
    $(SRCDIR)/core/wc.c \
    $(SRCDIR)/core/sort.c \
    $(SRCDIR)/core/grep.c \
    $(SRCDIR)/core/sed.c \
    $(SRCDIR)/core/find.c \
    $(SRCDIR)/core/xargs.c \
    $(SRCDIR)/core/basename.c \
    $(SRCDIR)/core/dirname.c \
    $(SRCDIR)/core/readlink.c \
    $(SRCDIR)/core/stat.c \
    $(SRCDIR)/core/date.c \
    $(SRCDIR)/core/printf.c \
    $(SRCDIR)/core/install.c \
    $(SRCDIR)/core/tr.c \
    $(SRCDIR)/core/cut.c \
    $(SRCDIR)/core/sh.c

SHELL_SRCS = \
    $(SRCDIR)/shell/vars.c \
    $(SRCDIR)/shell/lexer.c \
    $(SRCDIR)/shell/parser.c \
    $(SRCDIR)/shell/expand.c \
    $(SRCDIR)/shell/redirect.c \
    $(SRCDIR)/shell/exec.c \
    $(SRCDIR)/shell/job.c \
    $(SRCDIR)/shell/shell.c

MODULE_SRCS = \
    $(SRCDIR)/module/loader.c \
    $(SRCDIR)/module/registry.c

BATCH_SRCS = \
    $(SRCDIR)/batch/uring.c \
    $(SRCDIR)/batch/detect.c \
    $(SRCDIR)/batch/fallback.c

CACHE_SRCS = \
    $(SRCDIR)/cache/hashmap.c \
    $(SRCDIR)/cache/fscache.c

MAIN_SRCS = $(SRCDIR)/main.c

ALL_SRCS = $(UTIL_SRCS) $(CORE_SRCS) $(SHELL_SRCS) $(MODULE_SRCS) \
           $(BATCH_SRCS) $(CACHE_SRCS) $(MAIN_SRCS)

# Object files: mirror source tree under $(OBJDIR)
OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(ALL_SRCS))

# --- Targets ------------------------------------------------------------------
TARGET = $(BINDIR)/matchbox

.PHONY: all clean debug test install check \
        test-asan compat-test shell-test security-test \
        bench integration-test fuzz fuzz-run \
        analyze cppcheck test-valgrind cross-check \
        size-check install-hooks musl

all: $(TARGET)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Built: $@"
	@wc -c $@ | awk '{printf "Binary size: %s bytes (%.1fK)\n", $$1, $$1/1024}'

matchbox-static: CFLAGS += -DMATCHBOX_STATIC=1
matchbox-static: $(TARGET)

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS =
debug: LDLIBS = -ldl
debug: $(TARGET)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)/util $(OBJDIR)/core $(OBJDIR)/shell \
	         $(OBJDIR)/module $(OBJDIR)/batch $(OBJDIR)/cache

$(BINDIR):
	mkdir -p $(BINDIR)

# --- Install ------------------------------------------------------------------
PREFIX ?= /usr/local

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/matchbox
	$(TARGET) --install $(DESTDIR)$(PREFIX)/bin

# --- Tests --------------------------------------------------------------------
test: $(TARGET)
	@echo "=== Running unit tests ==="
	@bash tests/unit/run_tests.sh $(TARGET)

check: test

test-asan: CFLAGS = $(CFLAGS_DEBUG)
test-asan: LDFLAGS =
test-asan: LDLIBS = -ldl
test-asan: $(TARGET)
	@echo "=== Running unit tests (ASan+UBSan) ==="
	@bash tests/unit/run_tests.sh $(TARGET)

compat-test: $(TARGET)
	@echo "=== Running compatibility tests ==="
	@bash tests/compat/run.sh $(TARGET)

shell-test: $(TARGET)
	@echo "=== Running shell conformance tests ==="
	@for f in tests/unit/shell/test_*.sh; do \
	    echo "--- $$f ---"; \
	    bash $$f $(TARGET); \
	done

security-test: $(TARGET)
	@echo "=== Running security tests ==="
	@for f in tests/security/test_*.sh; do \
	    echo "--- $$f ---"; \
	    bash $$f $(TARGET); \
	done

bench: $(TARGET)
	@echo "=== Running benchmarks ==="
	@for f in tests/bench/bench_*.sh; do \
	    echo "--- $$f ---"; \
	    bash $$f $(TARGET); \
	done

integration-test: $(TARGET)
	@echo "=== Running integration tests ==="
	@bash tests/integration/run_integration.sh $(TARGET)

# --- Fuzz targets -------------------------------------------------------------
FUZZ_CC    = clang
FUZZ_FLAGS = -fsanitize=fuzzer,address,undefined -std=c11 -I$(SRCDIR)
FUZZ_DIR   = build/fuzz

fuzz: $(FUZZ_DIR)/fuzz_shell_lexer $(FUZZ_DIR)/fuzz_shell_parser \
      $(FUZZ_DIR)/fuzz_path_canon  $(FUZZ_DIR)/fuzz_grep_pattern  \
      $(FUZZ_DIR)/fuzz_sed_expr

$(FUZZ_DIR):
	mkdir -p $(FUZZ_DIR)

FUZZ_CORE = $(SRCDIR)/util/arena.c $(SRCDIR)/util/strbuf.c \
            $(SRCDIR)/util/path.c  $(SRCDIR)/util/error.c

$(FUZZ_DIR)/fuzz_shell_lexer: tests/fuzz/fuzz_shell_lexer.c \
    $(SRCDIR)/shell/lexer.c $(FUZZ_CORE) | $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_DIR)/fuzz_shell_parser: tests/fuzz/fuzz_shell_parser.c \
    $(SRCDIR)/shell/lexer.c $(SRCDIR)/shell/parser.c $(FUZZ_CORE) | $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_DIR)/fuzz_path_canon: tests/fuzz/fuzz_path_canon.c \
    $(SRCDIR)/util/path.c $(SRCDIR)/util/error.c $(SRCDIR)/util/strbuf.c | $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_DIR)/fuzz_grep_pattern: tests/fuzz/fuzz_grep_pattern.c \
    $(SRCDIR)/core/grep.c $(FUZZ_CORE) | $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_DIR)/fuzz_sed_expr: tests/fuzz/fuzz_sed_expr.c \
    $(SRCDIR)/core/sed.c $(FUZZ_CORE) | $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -o $@ $^

ITERS ?= 1000000
fuzz-run: fuzz
	@mkdir -p corpus
	@for f in $(FUZZ_DIR)/fuzz_*; do \
	    echo "Fuzzing $$f ($(ITERS) iterations)..."; \
	    $$f -max_total_time=60 -runs=$(ITERS) corpus/; \
	done

# --- Static analysis ----------------------------------------------------------
analyze: $(ALL_SRCS)
	clang --analyze $(CFLAGS_COMMON) -I$(SRCDIR) $(ALL_SRCS)

cppcheck: $(ALL_SRCS)
	cppcheck --enable=all --error-exitcode=1 \
	    --suppress=missingIncludeSystem \
	    -I$(SRCDIR) $(ALL_SRCS)

# --- Valgrind -----------------------------------------------------------------
test-valgrind: $(TARGET)
	@echo "=== Running unit tests under valgrind ==="
	valgrind --leak-check=full --error-exitcode=1 \
	    bash tests/unit/run_tests.sh $(TARGET)

# --- Cross compilation --------------------------------------------------------
cross-check:
	$(MAKE) CC=aarch64-linux-gnu-gcc LDFLAGS="-static" clean all

# --- Binary size check --------------------------------------------------------
SIZE_LIMIT = 1572864

size-check: $(TARGET)
	@SIZE=$$(wc -c < $(TARGET)); \
	if [ "$$SIZE" -gt "$(SIZE_LIMIT)" ]; then \
	    echo "FAIL: binary size $$SIZE bytes > 1.5MB limit"; \
	    exit 1; \
	else \
	    echo "OK: binary size $$SIZE bytes"; \
	fi

# --- PGO (requires pgo/ submodule) -----------------------------------------
PGO_DIR      = $(PWD)/pgo
PGO_RAW_DIR  = $(PGO_DIR)/profiles/raw
PGO_MERGED   = $(PGO_DIR)/profiles/merged.profdata
PGO_BINARY   = $(BINDIR)/matchbox-instrumented

pgo-instrument: clean
	$(MAKE) CC=$(CC) CFLAGS="$(CFLAGS_RELEASE) -fprofile-generate=$(PGO_RAW_DIR)" \
	        LDFLAGS="$(LDFLAGS)" TARGET=$(PGO_BINARY) all

pgo-collect: pgo-instrument
	cd $(PGO_DIR) && TARGET=$(PGO_BINARY) $(MAKE) workloads

pgo-merge:
	cd $(PGO_DIR) && $(MAKE) profile

pgo-build: pgo-merge
	$(MAKE) clean CC=$(CC) \
	        CFLAGS="$(CFLAGS_RELEASE) -fprofile-use=$(PGO_MERGED) -fprofile-correction" all

pgo-report:
	cd $(PGO_DIR) && TARGET=$(BINDIR)/matchbox $(MAKE) report

pgo-validate:
	cd $(PGO_DIR) && TARGET=$(BINDIR)/matchbox $(MAKE) validate

pgo: pgo-instrument pgo-collect pgo-build
	@echo "PGO build complete."

.PHONY: pgo pgo-instrument pgo-collect pgo-merge pgo-build pgo-report pgo-validate

# --- musl build (shipping binary) -----------------------------------------
# Requires musl-gcc (install: apt-get install musl-tools)
# Produces a fully static, stripped, PIE binary suitable for shipping.

musl:
	@command -v musl-gcc >/dev/null 2>&1 || \
	    { echo "ERROR: musl-gcc not found. Install with: apt-get install musl-tools"; exit 1; }
	$(MAKE) CC=musl-gcc \
	        CFLAGS="$(CFLAGS_RELEASE) -fPIE" \
	        LDFLAGS="-pie" \
	        LDLIBS="-ldl" \
	        TARGET=$(BINDIR)/matchbox-musl \
	        clean all
	strip -s $(BINDIR)/matchbox-musl
	@wc -c $(BINDIR)/matchbox-musl | awk '{printf "musl binary size: %s bytes (%.1fK)\n", $$1, $$1/1024}'
	@SIZE=$$(wc -c < $(BINDIR)/matchbox-musl); \
	if [ "$$SIZE" -gt "$(SIZE_LIMIT)" ]; then \
	    echo "FAIL: musl binary $$SIZE bytes > 1.5MB limit"; exit 1; \
	else echo "OK: musl binary within size limit"; fi

# --- Git hooks ----------------------------------------------------------------
install-hooks:
	mkdir -p .git/hooks
	cp .githooks/commit-msg .git/hooks/commit-msg
	chmod +x .git/hooks/commit-msg
	@echo "Git hooks installed."

# --- Clean --------------------------------------------------------------------
clean:
	rm -rf build/

# --- Dependencies -------------------------------------------------------------
$(OBJDIR)/util/strbuf.o:   $(SRCDIR)/util/strbuf.c  $(SRCDIR)/util/strbuf.h
$(OBJDIR)/util/path.o:     $(SRCDIR)/util/path.c    $(SRCDIR)/util/path.h
$(OBJDIR)/util/error.o:    $(SRCDIR)/util/error.c   $(SRCDIR)/util/error.h
$(OBJDIR)/util/arena.o:    $(SRCDIR)/util/arena.c   $(SRCDIR)/util/arena.h
$(OBJDIR)/util/platform.o: $(SRCDIR)/util/platform.c $(SRCDIR)/util/platform.h

UTIL_HDRS = $(SRCDIR)/util/error.h $(SRCDIR)/util/path.h $(SRCDIR)/util/strbuf.h

$(OBJDIR)/core/echo.o:     $(SRCDIR)/core/echo.c
$(OBJDIR)/core/mkdir.o:    $(SRCDIR)/core/mkdir.c    $(UTIL_HDRS)
$(OBJDIR)/core/cp.o:       $(SRCDIR)/core/cp.c       $(UTIL_HDRS)
$(OBJDIR)/core/cat.o:      $(SRCDIR)/core/cat.c      $(UTIL_HDRS)
$(OBJDIR)/core/chmod.o:    $(SRCDIR)/core/chmod.c    $(UTIL_HDRS)
$(OBJDIR)/core/mv.o:       $(SRCDIR)/core/mv.c       $(UTIL_HDRS)
$(OBJDIR)/core/rm.o:       $(SRCDIR)/core/rm.c       $(UTIL_HDRS)
$(OBJDIR)/core/ln.o:       $(SRCDIR)/core/ln.c       $(UTIL_HDRS)
$(OBJDIR)/core/touch.o:    $(SRCDIR)/core/touch.c    $(UTIL_HDRS)
$(OBJDIR)/core/head.o:     $(SRCDIR)/core/head.c     $(UTIL_HDRS)
$(OBJDIR)/core/tail.o:     $(SRCDIR)/core/tail.c     $(UTIL_HDRS)
$(OBJDIR)/core/wc.o:       $(SRCDIR)/core/wc.c       $(UTIL_HDRS)
$(OBJDIR)/core/sort.o:     $(SRCDIR)/core/sort.c     $(UTIL_HDRS)
$(OBJDIR)/core/grep.o:     $(SRCDIR)/core/grep.c     $(UTIL_HDRS)
$(OBJDIR)/core/sed.o:      $(SRCDIR)/core/sed.c      $(UTIL_HDRS)
$(OBJDIR)/core/find.o:     $(SRCDIR)/core/find.c     $(UTIL_HDRS)
$(OBJDIR)/core/xargs.o:    $(SRCDIR)/core/xargs.c    $(UTIL_HDRS)
$(OBJDIR)/core/basename.o: $(SRCDIR)/core/basename.c $(UTIL_HDRS)
$(OBJDIR)/core/dirname.o:  $(SRCDIR)/core/dirname.c  $(UTIL_HDRS)
$(OBJDIR)/core/readlink.o: $(SRCDIR)/core/readlink.c $(UTIL_HDRS)
$(OBJDIR)/core/stat.o:     $(SRCDIR)/core/stat.c     $(UTIL_HDRS)
$(OBJDIR)/core/date.o:     $(SRCDIR)/core/date.c     $(UTIL_HDRS)
$(OBJDIR)/core/printf.o:   $(SRCDIR)/core/printf.c   $(UTIL_HDRS)
$(OBJDIR)/core/install.o:  $(SRCDIR)/core/install.c  $(UTIL_HDRS)
$(OBJDIR)/core/tr.o:       $(SRCDIR)/core/tr.c       $(UTIL_HDRS)
$(OBJDIR)/core/cut.o:      $(SRCDIR)/core/cut.c      $(UTIL_HDRS)

SHELL_HDRS = $(SRCDIR)/shell/lexer.h $(SRCDIR)/shell/parser.h \
             $(SRCDIR)/shell/vars.h  $(SRCDIR)/shell/shell.h  \
             $(SRCDIR)/shell/expand.h $(SRCDIR)/shell/redirect.h \
             $(SRCDIR)/shell/exec.h  $(SRCDIR)/shell/job.h \
             $(SRCDIR)/util/arena.h

$(OBJDIR)/shell/vars.o:     $(SRCDIR)/shell/vars.c     $(SRCDIR)/shell/vars.h $(SRCDIR)/util/arena.h
$(OBJDIR)/shell/lexer.o:    $(SRCDIR)/shell/lexer.c    $(SRCDIR)/shell/lexer.h $(SRCDIR)/util/arena.h
$(OBJDIR)/shell/parser.o:   $(SRCDIR)/shell/parser.c   $(SHELL_HDRS)
$(OBJDIR)/shell/expand.o:   $(SRCDIR)/shell/expand.c   $(SHELL_HDRS) $(UTIL_HDRS)
$(OBJDIR)/shell/redirect.o: $(SRCDIR)/shell/redirect.c $(SHELL_HDRS) $(UTIL_HDRS)
$(OBJDIR)/shell/exec.o:     $(SRCDIR)/shell/exec.c     $(SHELL_HDRS) $(UTIL_HDRS) $(SRCDIR)/applets.h
$(OBJDIR)/shell/job.o:      $(SRCDIR)/shell/job.c      $(SRCDIR)/shell/job.h
$(OBJDIR)/shell/shell.o:    $(SRCDIR)/shell/shell.c    $(SHELL_HDRS) $(UTIL_HDRS)

$(OBJDIR)/module/loader.o:   $(SRCDIR)/module/loader.c   $(SRCDIR)/module/loader.h
$(OBJDIR)/module/registry.o: $(SRCDIR)/module/registry.c $(SRCDIR)/module/registry.h

$(OBJDIR)/batch/uring.o:    $(SRCDIR)/batch/uring.c    $(SRCDIR)/batch/uring.h
$(OBJDIR)/batch/detect.o:   $(SRCDIR)/batch/detect.c   $(SRCDIR)/batch/detect.h
$(OBJDIR)/batch/fallback.o: $(SRCDIR)/batch/fallback.c $(SRCDIR)/batch/fallback.h

$(OBJDIR)/cache/hashmap.o:  $(SRCDIR)/cache/hashmap.c  $(SRCDIR)/cache/hashmap.h
$(OBJDIR)/cache/fscache.o:  $(SRCDIR)/cache/fscache.c  $(SRCDIR)/cache/fscache.h

$(OBJDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/applets.h $(SRCDIR)/util/error.h
