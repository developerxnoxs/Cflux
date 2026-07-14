# Makefile for Flux - fallback when cmake is not available
CC      ?= gcc
CFLAGS  := -std=gnu17 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
           -Wno-unused-variable -Iinclude -g -O0
LDFLAGS := -lm

BUILD   := build_make
LIB_OBJ := $(BUILD)/util.o    \
            $(BUILD)/chunk.o  \
            $(BUILD)/object.o \
            $(BUILD)/gc.o     \
            $(BUILD)/lexer.o  \
            $(BUILD)/ast.o    \
            $(BUILD)/parser.o \
            $(BUILD)/compiler.o \
            $(BUILD)/vm.o    \
            $(BUILD)/runtime.o \
            $(BUILD)/stdlib.o \
            $(BUILD)/api.o

.PHONY: all clean test

all: $(BUILD)/flux $(BUILD)/libflux.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/util.o:     src/util/util.c       | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/chunk.o:    src/vm/chunk.c        | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/object.o:   src/object/object.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gc.o:       src/gc/gc.c           | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lexer.o:    src/lexer/lexer.c     | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/ast.o:      src/ast/ast.c         | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/parser.o:   src/parser/parser.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/compiler.o: src/compiler/compiler.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/vm.o:       src/vm/vm.c           | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/runtime.o:  src/runtime/runtime.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stdlib.o:   src/stdlib/stdlib.c   | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/api.o:      src/api/api.c         | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libflux.a: $(LIB_OBJ)
	ar rcs $@ $^

$(BUILD)/flux: src/main.c $(BUILD)/libflux.a
	$(CC) $(CFLAGS) $< -L$(BUILD) -lflux $(LDFLAGS) -o $@

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
