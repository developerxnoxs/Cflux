# Makefile for Flux
CC      ?= gcc
CFLAGS  := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wpedantic \
           -Wno-unused-parameter -Wno-unused-variable -Iinclude -g -O0
LDFLAGS := -lm -ldl

BUILD   := build_make

# Core VM / compiler objects
VM_OBJ  := $(BUILD)/util.o    \
            $(BUILD)/chunk.o  \
            $(BUILD)/object.o \
            $(BUILD)/gc.o     \
            $(BUILD)/lexer.o  \
            $(BUILD)/ast.o    \
            $(BUILD)/parser.o \
            $(BUILD)/compiler.o \
            $(BUILD)/vm.o    \
            $(BUILD)/runtime.o \
            $(BUILD)/api.o

# Standard library objects — only `core` is statically linked; every other
# module (math, io, time, fs, os, sys, json) is built as its own lazily-
# loaded .so under stdlib/<name>/ (see the `stdlib` target below).
STDLIB_OBJ := \
            $(BUILD)/stdlib.o       \
            $(BUILD)/stdlib_core.o

LIB_OBJ := $(VM_OBJ) $(STDLIB_OBJ)

.PHONY: all clean test extensions stdlib

all: $(BUILD)/flux $(BUILD)/libflux.a stdlib extensions

# ----- stdlib modules (.so plugins, part of `all`) -----
# Each subfolder under stdlib/ has its own Makefile that builds
# stdlib/<name>/lib<name>.so. These are Flux's own official modules
# (math, io, time, fs, os, sys, json) — unlike extension/, they only depend
# on libc/libm, so they're built by default whenever `all` runs, and loaded
# lazily the first time a script does `import <name>` (see vm.c).
stdlib:
	@for d in stdlib/*/; do \
		if [ -f "$$d""Makefile" ]; then \
			$(MAKE) -s --no-print-directory -C "$$d" FLUX_INCLUDE=$(CURDIR)/include || exit 1; \
		fi; \
	done

# ----- native extensions (part of `all`) -----
# Each subfolder under extension/ has its own Makefile that builds
# extension/<name>/lib<name>.so, dlopen()'d lazily the first time a script
# does `import <name>` (see try_load_native_extension() in vm.c). They depend
# on external system libraries (e.g. libpq for postgresql, declared in
# replit.nix); if one of those libraries isn't installed in a given
# environment, that extension's build fails loudly here rather than at
# `import` time. Run `make extensions` on its own to rebuild just these.
extensions:
	@for d in extension/*/; do \
		if [ -f "$${d}Makefile" ]; then \
			$(MAKE) -s --no-print-directory -C "$$d" FLUX_INCLUDE=$(CURDIR)/include || exit 1; \
		fi; \
	done

$(BUILD):
	mkdir -p $(BUILD)

# ----- VM / compiler -----
$(BUILD)/util.o:     src/util/util.c         | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/chunk.o:    src/vm/chunk.c           | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/object.o:   src/object/object.c      | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gc.o:       src/gc/gc.c              | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lexer.o:    src/lexer/lexer.c        | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/ast.o:      src/ast/ast.c            | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/parser.o:   src/parser/parser.c      | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/compiler.o: src/compiler/compiler.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/vm.o:       src/vm/vm.c              | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/runtime.o:  src/runtime/runtime.c    | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/api.o:      src/api/api.c            | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ----- stdlib core (only statically-linked module) -----
$(BUILD)/stdlib.o:      src/stdlib/stdlib.c       | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_core.o: src/stdlib/stdlib_core.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ----- link targets -----
$(BUILD)/libflux.a: $(LIB_OBJ)
	ar rcs $@ $^

# -rdynamic exports the interpreter's own symbols (object_dict_new,
# dict_set, vm_runtime_error, ...) into the dynamic symbol table so that
# native extensions loaded via dlopen() (see extension/) can resolve them —
# without it, every symbol lookup in a loaded .so fails at runtime.
$(BUILD)/flux: src/main.c $(BUILD)/libflux.a
	$(CC) $(CFLAGS) -rdynamic $< -L$(BUILD) -lflux $(LDFLAGS) -o $@

# ----- tests -----
$(BUILD)/test_lexer: tests/test_lexer.c $(BUILD)/libflux.a
	$(CC) $(CFLAGS) $< -L$(BUILD) -lflux $(LDFLAGS) -o $@

$(BUILD)/test_vm: tests/test_vm.c $(BUILD)/libflux.a
	$(CC) $(CFLAGS) $< -L$(BUILD) -lflux $(LDFLAGS) -o $@

$(BUILD)/test_gc: tests/test_gc.c $(BUILD)/libflux.a
	$(CC) $(CFLAGS) $< -L$(BUILD) -lflux $(LDFLAGS) -o $@

test: $(BUILD)/test_lexer $(BUILD)/test_vm $(BUILD)/test_gc
	@echo ""
	@echo "=== Running Lexer Tests ==="
	@$(BUILD)/test_lexer
	@echo ""
	@echo "=== Running VM Tests ==="
	@$(BUILD)/test_vm
	@echo ""
	@echo "=== Running GC Tests ==="
	@$(BUILD)/test_gc

clean:
	rm -rf $(BUILD)
