# Makefile for the Flux "postgresql" native extension.
#
# Builds libpostgresql.so, which the Flux VM dlopen()s the moment a script
# does `import postgresql` and no postgresql.flx file exists (see
# include/flux/extension.h and src/vm/vm.c's try_load_native_extension).
#
# Usage:
#   make -C extension/postgresql
# or, from the project root:
#   make extensions
#
# Requires libpq's headers/library (Nix package names: `postgresql`, `libpq`).
CC       ?= gcc
FLUX_INCLUDE ?= ../../include
CFLAGS   := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
            -fPIC -I$(FLUX_INCLUDE) -g -O0 -U_FORTIFY_SOURCE -Wno-cpp
LDFLAGS  := -lpq -shared

TARGET := libpostgresql.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): postgresql_ext.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
