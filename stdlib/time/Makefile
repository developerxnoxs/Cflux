# Builds stdlib/time/libtime.so — the lazily-loaded `time` module.
CC           ?= gcc
FLUX_INCLUDE ?= ../../include
CFLAGS       := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
                -fPIC -I$(FLUX_INCLUDE) -g -O0
LDFLAGS      :=

TARGET := libtime.so
SRC    := time_module.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -shared -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
