# Makefile for Flux
CC      ?= gcc

# Install locations for `make install` (override e.g. `make install PREFIX=$HOME/.local`).
# DESTDIR is a separate staging-root prefix, honored by convention for packaging.
PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin
SHAREDIR:= $(DESTDIR)$(PREFIX)/share/flux

CFLAGS  := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wpedantic \
           -Wno-unused-parameter -Wno-unused-variable -Iinclude -g -O0 \
           -U_FORTIFY_SOURCE -Wno-cpp \
           -DFLUX_SHARE_DIR=\"$(PREFIX)/share/flux\"
LDFLAGS := -lm -ldl -luv

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

# CLI toolchain objects (compiled into the flux binary only, not libflux.a)
TOOL_OBJ := $(BUILD)/tools_fmt.o   \
             $(BUILD)/tools_lint.o  \
             $(BUILD)/tools_doc.o   \
             $(BUILD)/tools_pkg.o   \
             $(BUILD)/tools_build.o

.PHONY: all clean test extensions stdlib install uninstall

all: $(BUILD)/flux $(BUILD)/libflux.a stdlib extensions

# ----- stdlib modules (.so plugins, part of `all`) -----
# Each subfolder under stdlib/ has its own Makefile that builds
# stdlib/<name>/lib<name>.so. These are Flux's own official modules
# (math, io, time, fs, os, sys, json, async) — unlike extension/, they only
# depend on libc/libm (async also links libuv), so they're built by default
# whenever `all` runs, and loaded lazily the first time a script does
# `import <name>` (see vm.c).
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

# ----- CLI toolchain (fmt, lint, doc, pkg, build) -----
$(BUILD)/tools_fmt.o:   src/tools/fmt.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/tools_lint.o:  src/tools/lint.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/tools_doc.o:   src/tools/doc.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/tools_pkg.o:   src/tools/pkg.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/tools_build.o: src/tools/build.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# -rdynamic exports the interpreter's own symbols (object_dict_new,
# dict_set, vm_runtime_error, ...) into the dynamic symbol table so that
# native extensions loaded via dlopen() (see extension/) can resolve them —
# without it, every symbol lookup in a loaded .so fails at runtime.
$(BUILD)/flux: src/main.c $(TOOL_OBJ) $(BUILD)/libflux.a
	$(CC) $(CFLAGS) -rdynamic $< $(TOOL_OBJ) -L$(BUILD) -lflux $(LDFLAGS) -o $@

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

# ----- system-wide install -----
# Installs the interpreter to $(BINDIR)/flux and copies the stdlib/ and
# extension/ module trees (built .so's included) to $(SHAREDIR), so `flux`
# and its `import math` / `import io` / ... keep working when run from any
# directory, not just this source tree. The binary is compiled with
# FLUX_SHARE_DIR baked in (see CFLAGS above) so it knows where to fall back
# to when a module isn't found next to the running script or in the cwd.
install: all
	install -d "$(BINDIR)"
	install -m 755 $(BUILD)/flux "$(BINDIR)/flux"
	install -d "$(SHAREDIR)"
	rm -rf "$(SHAREDIR)/stdlib" "$(SHAREDIR)/extension"
	cp -r stdlib "$(SHAREDIR)/stdlib"
	cp -r extension "$(SHAREDIR)/extension"
	@echo "Installed flux to $(BINDIR)/flux"
	@echo "Installed stdlib/extension modules to $(SHAREDIR)"

uninstall:
	rm -f "$(BINDIR)/flux"
	rm -rf "$(SHAREDIR)"
	@echo "Removed $(BINDIR)/flux and $(SHAREDIR)"
