# Ernest Makefile.
#
# JPL Power-of-Ten flavoured. Builds with the warning regime turned up,
# the GCC analyser running alongside the compiler, automatic dependency
# tracking, a separate build directory, and a debug variant that wears
# the sanitizers like a high-vis vest. Make is friendly to the curious
# (try `make help`) and unfriendly to bugs (try `make` and watch).

# ----- Toolchain -----
CC      := gcc
AR      := ar

# Windows wants its binaries with the .exe on the end. Other operating
# systems are less particular. We ask the environment which neighbourhood
# we're in and adjust accordingly.
ifeq ($(OS),Windows_NT)
    EXE         := .exe
    HAVE_FORTIFY := 0
else
    EXE         :=
    HAVE_FORTIFY := 1
endif

# ----- Layout -----
SRC_DIR   := src
DEMO_DIR  := demos
BUILD_DIR := build
SRC       := $(wildcard $(SRC_DIR)/*.c)
DEMOS     := $(wildcard $(DEMO_DIR)/*.c)
OBJ       := $(SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o) \
             $(DEMOS:$(DEMO_DIR)/%.c=$(BUILD_DIR)/demos_%.o)
DEP       := $(OBJ:.o=.d)
BIN       := ernest$(EXE)

# ----- Warning regime -----
# Each one of these catches a different way for a program to embarrass
# itself in production. Werror promotes every warning to an error,
# because a warning ignored is a bug postponed.
WARNINGS := \
    -Wall \
    -Wextra \
    -Wpedantic \
    -pedantic-errors \
    -Werror \
    -Wshadow \
    -Wcast-qual \
    -Wcast-align \
    -Wpointer-arith \
    -Wfloat-equal \
    -Wundef \
    -Wstrict-aliasing \
    -Wstrict-prototypes \
    -Wmissing-prototypes \
    -Wmissing-declarations \
    -Wredundant-decls \
    -Wswitch-default \
    -Wwrite-strings \
    -Winit-self \
    -Wformat=2 \
    -Wnull-dereference \
    -Wdouble-promotion \
    -Wlogical-op \
    -Wjump-misses-init \
    -Wold-style-definition

# GCC's built-in static analyser. Catches a different class of bug
# from the warnings above: things like leaks, double-frees,
# tainted-data flows. Slower compile, but the time is well spent.
ANALYSE := -fanalyzer

# ----- Hardening flags -----
# Stack canaries, control-flow checks, and zero-initialised stack
# variables. _FORTIFY_SOURCE only applies under glibc, so we gate it
# on the host being not-Windows.
HARDEN := \
    -fstack-protector-strong \
    -ftrivial-auto-var-init=zero
ifeq ($(HAVE_FORTIFY),1)
    HARDEN += -D_FORTIFY_SOURCE=2
endif

# ----- Dependency tracking -----
# -MMD emits .d files listing every header an object depends on.
# -MP adds phony rules so deleting a header doesn't break the build.
# The -include line further down quietly pulls all of them in.
DEPGEN := -MMD -MP

# ----- Standard + base flags -----
STD     := -std=c99
CFLAGS  := $(STD) -O2 -g $(WARNINGS) $(HARDEN) $(DEPGEN) $(ANALYSE)
LDFLAGS :=
LDLIBS  := -lm

# JPL Power of Ten requires runtime assertions to remain active.
# We deliberately do NOT define NDEBUG. Release builds keep their
# safety net.

# ----- Default target -----
.PHONY: all
all: $(BIN)

# ----- Link -----
$(BIN): $(OBJ)
	@printf '  %-6s %s\n' 'LINK' '$@'
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ----- Compile -----
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@printf '  %-6s %s\n' 'CC' '$<'
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/demos_%.o: $(DEMO_DIR)/%.c | $(BUILD_DIR)
	@printf '  %-6s %s\n' 'CC' '$<'
	@$(CC) $(CFLAGS) -c -o $@ $<

# ----- Build directory -----
$(BUILD_DIR):
	@mkdir -p $@

# ----- Auto-generated dependencies -----
# The leading dash tells make not to mind if the .d files don't exist
# yet, which they won't on a clean build.
-include $(DEP)

# ----- Debug build -----
# Sanitizers and full debug info, no optimisation. UBSAN tends to work
# everywhere GCC does; ASAN is happy on Linux and macOS, less reliable
# on MinGW. If `make debug` complains on Windows, comment out the
# sanitizer flag and try again.
.PHONY: debug
debug: CFLAGS  := $(STD) -O0 -g3 $(WARNINGS) $(DEPGEN) -fsanitize=undefined,address
debug: LDFLAGS := -fsanitize=undefined,address
debug: clean $(BIN)

# ----- Run -----
.PHONY: run
run: $(BIN)
	@./$(BIN)

# ----- Quick smoke test -----
# Build, run, check the exit code is zero. Doesn't validate the
# histogram, just that the program completes without sulking.
.PHONY: smoke
smoke: $(BIN)
	@./$(BIN) > /dev/null && echo 'smoke: ok' || (echo 'smoke: FAIL' && exit 1)

# ----- Static analysis (optional, requires cppcheck) -----
.PHONY: cppcheck
cppcheck:
	@command -v cppcheck > /dev/null 2>&1 || (echo 'cppcheck not installed' && exit 1)
	cppcheck --enable=all --std=c99 --inconclusive --suppress=missingIncludeSystem $(SRC_DIR)

# ----- Clean -----
.PHONY: clean
clean:
	@rm -rf $(BUILD_DIR) $(BIN)

# ----- Help -----
.PHONY: help
help:
	@echo 'Ernest build targets:'
	@echo '  all       build the release binary (default)'
	@echo '  debug     build with sanitizers, no optimisation'
	@echo '  run       build and run the Bell state demo'
	@echo '  smoke     build and check the demo exits cleanly'
	@echo '  cppcheck  run cppcheck static analysis (if installed)'
	@echo '  clean     remove all build artefacts'
	@echo '  help      this message'
