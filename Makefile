# Makefile for Flux
CC      ?= gcc
CFLAGS  := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wpedantic \
           -Wno-unused-parameter -Wno-unused-variable -Iinclude -g -O0
LDFLAGS := -lm

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

# Standard library objects — one per module
STDLIB_OBJ := \
            $(BUILD)/stdlib.o       \
            $(BUILD)/stdlib_core.o  \
            $(BUILD)/stdlib_math.o  \
            $(BUILD)/stdlib_io.o    \
            $(BUILD)/stdlib_time.o  \
            $(BUILD)/stdlib_fs.o    \
            $(BUILD)/stdlib_os.o    \
            $(BUILD)/stdlib_sys.o   \
            $(BUILD)/stdlib_json.o

LIB_OBJ := $(VM_OBJ) $(STDLIB_OBJ)

.PHONY: all clean test

all: $(BUILD)/flux $(BUILD)/libflux.a

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

# ----- stdlib modules -----
$(BUILD)/stdlib.o:      src/stdlib/stdlib.c       | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_core.o: src/stdlib/stdlib_core.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_math.o: src/stdlib/stdlib_math.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_io.o:   src/stdlib/stdlib_io.c    | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_time.o: src/stdlib/stdlib_time.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_fs.o:   src/stdlib/stdlib_fs.c    | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_os.o:   src/stdlib/stdlib_os.c    | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_sys.o:  src/stdlib/stdlib_sys.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib_json.o: src/stdlib/stdlib_json.c  | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ----- link targets -----
$(BUILD)/libflux.a: $(LIB_OBJ)
	ar rcs $@ $^

$(BUILD)/flux: src/main.c $(BUILD)/libflux.a
	$(CC) $(CFLAGS) $< -L$(BUILD) -lflux $(LDFLAGS) -o $@

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
