# Builds stdlib/math/libmath.so — the lazily-loaded `math` module.
# Invoked by the root Makefile's `stdlib` target (see ../../Makefile), or
# directly:  make -C stdlib/math FLUX_INCLUDE=/path/to/include

CC           ?= gcc
FLUX_INCLUDE ?= ../../include
CFLAGS       := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
                -fPIC -I$(FLUX_INCLUDE) -g -O0 -U_FORTIFY_SOURCE -Wno-cpp
LDFLAGS      := -lm

TARGET := libmath.so
SRC    := math_module.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -shared -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
