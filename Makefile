# Makefile -- matchbox container build runtime
# Dual build: musl-static (release) and glibc-dynamic (release-glibc).
# Auto-detects compiler and libc; defaults to glibc if musl-gcc is absent.

# --- Version ------------------------------------------------------------------
VERSION := 0.2.0

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

# --- LTO flag: thin (clang) or auto-parallel (gcc) ----------------------------
ifneq (,$(findstring clang,$(CC)))
    LTO_FLAG = -flto=thin
else
    LTO_FLAG = -flto=auto
endif

# --- Architecture baseline (overridable: make MARCH=x86-64-v3 for newer CPUs) -
MARCH ?= x86-64-v2
ARCH  := $(shell uname -m)

ifeq ($(ARCH),x86_64)
    MARCH_FLAG = -march=$(MARCH)
else ifeq ($(ARCH),aarch64)
    MARCH_FLAG = -march=armv8-a+simd
else
    MARCH_FLAG =
endif

# --- Libc detection -----------------------------------------------------------
ifeq ($(CC),musl-gcc)
    LIBC_DEFINE = -DMATCHBOX_LIBC_MUSL=1
else ifneq (,$(wildcard /lib/ld-musl-*.so*))
    LIBC_DEFINE = -DMATCHBOX_LIBC_MUSL=1
else
    LIBC_DEFINE = -DMATCHBOX_LIBC_GLIBC=1
endif

# --- Flags --------------------------------------------------------------------
CFLAGS_COMMON  = -std=c11 -Wall -Wextra -Werror -pedantic \
                 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                 -DMATCHBOX_VERSION=\"$(VERSION)\" \
                 -fstack-protector-strong \
                 $(LIBC_DEFINE)

CFLAGS_RELEASE = $(CFLAGS_COMMON) \
                 -O2 $(LTO_FLAG) -fPIE \
                 $(MARCH_FLAG) \
                 -ffunction-sections \
                 -fdata-sections \
                 -fmerge-all-constants \
                 -fno-unwind-tables \
                 -fno-asynchronous-unwind-tables \
                 -fvisibility=hidden

CFLAGS_DEBUG   = $(CFLAGS_COMMON) \
                 -O0 -g3 \
                 -fno-omit-frame-pointer \
                 -fsanitize=address,undefined

CFLAGS ?= $(CFLAGS_RELEASE)

# --- Reproducible builds: pass SOURCE_DATE_EPOCH as build date ----------------
ifdef SOURCE_DATE_EPOCH
    CFLAGS_COMMON += -Wdate-time \
        -DMATCHBOX_BUILD_DATE=\"$(shell \
            date -u -d @$(SOURCE_DATE_EPOCH) '+%Y-%m-%d' 2>/dev/null || \
            date -u -r $(SOURCE_DATE_EPOCH) '+%Y-%m-%d')\"
endif

# --- LDFLAGS ------------------------------------------------------------------
LDFLAGS_MUSL  = -static-pie \
                -Wl,--gc-sections \
                -Wl,--as-needed \
                -Wl,-z,relro \
                -Wl,-z,now \
                -Wl,-z,noexecstack \
                -Wl,--build-id=sha1

LDFLAGS_GLIBC = -pie \
                -Wl,--gc-sections \
                -Wl,--as-needed \
                -Wl,-z,relro \
                -Wl,-z,now \
                -Wl,-z,noexecstack \
                -Wl,--build-id=sha1

ifdef MUSL_GCC
    LDFLAGS ?= $(LDFLAGS_MUSL)
    LINK_TYPE = static
else
    LDFLAGS ?= $(LDFLAGS_GLIBC)
    LINK_TYPE = dynamic
endif

# Link with dl for module loading (dlopen)
LDLIBS = -ldl

# --- Directories --------------------------------------------------------------
SRCDIR   = src
OBJDIR   = build/obj
BINDIR   = build/bin

# --- Sources ------------------------------------------------------------------
# Architecture detection for vectorised linescan
ifeq ($(ARCH),x86_64)
    LINESCAN_SRC = $(SRCDIR)/util/linescan_avx2.c
    LINESCAN_OBJ = $(OBJDIR)/util/linescan_avx2.o
    CFLAGS_LINESCAN = -mavx2
else ifeq ($(ARCH),aarch64)
    LINESCAN_SRC = $(SRCDIR)/util/linescan_neon.c
    LINESCAN_OBJ = $(OBJDIR)/util/linescan_neon.o
    CFLAGS_LINESCAN =
else
    LINESCAN_SRC = $(SRCDIR)/util/linescan_scalar.c
    LINESCAN_OBJ = $(OBJDIR)/util/linescan_scalar.o
    CFLAGS_LINESCAN =
endif

REGEX_SRCS = \
    $(SRCDIR)/util/regex/classify.c \
    $(SRCDIR)/util/regex/charclass_re.c \
    $(SRCDIR)/util/regex/compile.c \
    $(SRCDIR)/util/regex/parse.c \
    $(SRCDIR)/util/regex/thompson.c \
    $(SRCDIR)/util/regex/mb_regex.c

UTIL_SRCS = \
    $(SRCDIR)/util/strbuf.c \
    $(SRCDIR)/util/path.c \
    $(SRCDIR)/util/error.c \
    $(SRCDIR)/util/arena.c \
    $(SRCDIR)/util/platform.c \
    $(SRCDIR)/util/charclass.c \
    $(SRCDIR)/util/intern.c \
    $(LINESCAN_SRC) \
    $(REGEX_SRCS)

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
    $(SRCDIR)/core/sh.c \
    $(SRCDIR)/core/mktemp.c \
    $(SRCDIR)/core/tee.c \
    $(SRCDIR)/core/env.c \
    $(SRCDIR)/core/realpath.c \
    $(SRCDIR)/core/sha256sum.c

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

# --- Per-file optimisation overrides ------------------------------------------
# HOT_OBJS: compiled at -O3 in release builds
HOT_OBJS = \
    $(OBJDIR)/shell/lexer.o \
    $(OBJDIR)/core/cp.o \
    $(OBJDIR)/core/grep.o \
    $(OBJDIR)/core/sed.o \
    $(OBJDIR)/core/sort.o \
    $(OBJDIR)/core/wc.o \
    $(OBJDIR)/core/find.o \
    $(OBJDIR)/core/cat.o \
    $(OBJDIR)/core/mkdir.o \
    $(OBJDIR)/core/chmod.o \
    $(OBJDIR)/util/charclass.o \
    $(LINESCAN_OBJ) \
    $(OBJDIR)/util/arena.o \
    $(OBJDIR)/util/strbuf.o \
    $(OBJDIR)/util/intern.o \
    $(OBJDIR)/cache/fscache.o \
    $(OBJDIR)/cache/hashmap.o \
    $(OBJDIR)/util/regex/thompson.o \
    $(OBJDIR)/util/regex/classify.o \
    $(OBJDIR)/util/regex/compile.o

# COLD_OBJS: compiled at -Os in release builds (rarely executed paths)
COLD_OBJS = \
    $(OBJDIR)/module/loader.o \
    $(OBJDIR)/module/registry.o \
    $(OBJDIR)/util/error.o \
    $(OBJDIR)/util/platform.o

# PERFILE_OPT is evaluated lazily at recipe time (= not :=) so it picks up
# the target-specific RELEASE_OPT set by all/release/release-glibc.
$(HOT_OBJS):  PERFILE_OPT = $(if $(RELEASE_OPT),-O3)
$(COLD_OBJS): PERFILE_OPT = $(if $(RELEASE_OPT),-Os)

# --- Targets ------------------------------------------------------------------
TARGET = $(BINDIR)/matchbox

.PHONY: all clean debug release release-glibc release-docker test install check \
        test-asan compat-test shell-test security-test \
        bench integration-test fuzz fuzz-run \
        analyze cppcheck test-valgrind cross-check \
        size-check install-hooks musl \
        stress-test edge-test

all: RELEASE_OPT = 1
all: $(TARGET)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Built: $@"
	@wc -c $@ | awk '{printf "Binary size: %s bytes (%.1fK)\n", $$1, $$1/1024}'

# --- Release targets ----------------------------------------------------------

release: CFLAGS = $(CFLAGS_RELEASE) -DMATCHBOX_BUILD_STATIC=1 -DMATCHBOX_LIBC_MUSL=1
release: LDFLAGS = $(LDFLAGS_MUSL)
release: RELEASE_OPT = 1
release: $(TARGET)
	strip -s $(TARGET)
	@echo "Release (musl static): $$(wc -c < $(TARGET)) bytes"

release-glibc: CFLAGS = $(CFLAGS_RELEASE) -DMATCHBOX_BUILD_DYNAMIC=1 -DMATCHBOX_LIBC_GLIBC=1
release-glibc: LDFLAGS = $(LDFLAGS_GLIBC)
release-glibc: RELEASE_OPT = 1
release-glibc: $(TARGET)
	strip -s $(TARGET)
	@echo "Release (glibc dynamic): $$(wc -c < $(TARGET)) bytes"

release-docker:
	docker run --rm -v $(PWD):/src -w /src alpine:latest sh -c \
	    "apk add --no-cache gcc musl-dev make linux-headers && make release"

# --- Debug build (ASan/UBSan) -------------------------------------------------
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS =
debug: LDLIBS = -ldl
debug: $(TARGET)

# --- Legacy musl alias --------------------------------------------------------
musl: release

# --- Compile rule (with auto-generated header dependencies) ------------------
DEPFLAGS = -MMD -MP
DEPS     = $(OBJS:.o=.d)
-include $(DEPS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PERFILE_OPT) $(DEPFLAGS) -I$(SRCDIR) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)/util $(OBJDIR)/util/regex $(OBJDIR)/core \
	         $(OBJDIR)/shell $(OBJDIR)/module $(OBJDIR)/batch $(OBJDIR)/cache

$(BINDIR):
	mkdir -p $(BINDIR)

# --- Install ------------------------------------------------------------------
PREFIX ?= /usr/local

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/matchbox
	$(TARGET) --install $(DESTDIR)$(PREFIX)/bin

# --- Tests --------------------------------------------------------------------
# C unit test binaries
TEST_CHARCLASS = build/bin/test_charclass
TEST_LINESCAN  = build/bin/test_linescan
TEST_REGEX     = build/bin/test_regex

REGEX_TEST_SRCS = \
    $(SRCDIR)/util/regex/classify.c \
    $(SRCDIR)/util/regex/charclass_re.c \
    $(SRCDIR)/util/regex/compile.c \
    $(SRCDIR)/util/regex/parse.c \
    $(SRCDIR)/util/regex/thompson.c \
    $(SRCDIR)/util/regex/mb_regex.c

$(TEST_CHARCLASS): tests/unit/test_charclass.c $(SRCDIR)/util/charclass.c \
                  $(SRCDIR)/util/charclass.h | $(BINDIR)
	$(CC) $(CFLAGS_COMMON) -I$(SRCDIR) -o $@ \
	    tests/unit/test_charclass.c $(SRCDIR)/util/charclass.c

$(TEST_LINESCAN): tests/unit/test_linescan.c $(SRCDIR)/util/linescan_scalar.c \
                  $(SRCDIR)/util/linescan.h | $(BINDIR)
	$(CC) $(CFLAGS_COMMON) -I$(SRCDIR) -o $@ \
	    tests/unit/test_linescan.c $(SRCDIR)/util/linescan_scalar.c

$(TEST_REGEX): tests/unit/test_regex.c $(REGEX_TEST_SRCS) | $(BINDIR)
	@mkdir -p $(OBJDIR)/util/regex
	$(CC) $(CFLAGS_COMMON) -I$(SRCDIR) -o $@ \
	    tests/unit/test_regex.c $(REGEX_TEST_SRCS)

test: $(TARGET) $(TEST_CHARCLASS) $(TEST_LINESCAN) $(TEST_REGEX)
	@echo "=== Running unit tests ==="
	@bash tests/unit/run_tests.sh $(TARGET)
	@echo "--- test_charclass (C) ---"
	@$(TEST_CHARCLASS) && echo "PASS: test_charclass"
	@echo "--- test_linescan (C) ---"
	@$(TEST_LINESCAN) && echo "PASS: test_linescan"
	@echo "--- test_regex (C) ---"
	@$(TEST_REGEX) && echo "PASS: test_regex"

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
	    (ulimit -v 4194304; ulimit -f 1048576; ulimit -t 300; ulimit -d 2097152; bash $$f $(TARGET)); \
	done

integration-test: $(TARGET)
	@echo "=== Running integration tests ==="
	@bash tests/integration/run_integration.sh $(TARGET)

stress-test: $(TARGET)
	@echo "=== Running stress tests ==="
	@bash tests/stress/run_stress.sh 100

edge-test: $(TARGET)
	@echo "=== Running edge case tests ==="
	@bash tests/edge/run_edge.sh $(TARGET)

check: $(TARGET)
	@sh check.sh

# --- Module build -------------------------------------------------------------
MODULES_DIR = modules
MODULES_OUT = build/modules
MODULE_SRCS = $(wildcard $(MODULES_DIR)/*.c)
MODULE_SOS  = $(patsubst $(MODULES_DIR)/%.c,$(MODULES_OUT)/%.so,$(MODULE_SRCS))

modules: $(MODULE_SOS)

$(MODULES_OUT)/%.so: $(MODULES_DIR)/%.c matchbox_module.h | $(MODULES_OUT)
	$(CC) -shared -fPIC -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -I$(SRCDIR) -o $@ $<

$(MODULES_OUT):
	mkdir -p $(MODULES_OUT)

install-modules: modules
	install -d $(DESTDIR)/usr/lib/matchbox/modules
	install -m 644 $(MODULES_OUT)/*.so $(DESTDIR)/usr/lib/matchbox/modules/

clean-modules:
	rm -rf $(MODULES_OUT)

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
	    $$f -max_total_time=60 -runs=$(ITERS) -rss_limit_mb=2048 -max_len=65536 corpus/; \
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

size-check: release
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
$(OBJDIR)/util/strbuf.o:    $(SRCDIR)/util/strbuf.c    $(SRCDIR)/util/strbuf.h
$(OBJDIR)/util/path.o:      $(SRCDIR)/util/path.c      $(SRCDIR)/util/path.h
$(OBJDIR)/util/error.o:     $(SRCDIR)/util/error.c     $(SRCDIR)/util/error.h
$(OBJDIR)/util/arena.o:     $(SRCDIR)/util/arena.c     $(SRCDIR)/util/arena.h
$(OBJDIR)/util/platform.o:  $(SRCDIR)/util/platform.c  $(SRCDIR)/util/platform.h
$(OBJDIR)/util/charclass.o:      $(SRCDIR)/util/charclass.c $(SRCDIR)/util/charclass.h
$(OBJDIR)/util/linescan_avx2.o: $(SRCDIR)/util/linescan_avx2.c $(SRCDIR)/util/linescan.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CFLAGS_LINESCAN) $(PERFILE_OPT) -I$(SRCDIR) -c $< -o $@
$(OBJDIR)/util/linescan_neon.o:   $(SRCDIR)/util/linescan_neon.c   $(SRCDIR)/util/linescan.h
$(OBJDIR)/util/linescan_scalar.o: $(SRCDIR)/util/linescan_scalar.c $(SRCDIR)/util/linescan.h

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
