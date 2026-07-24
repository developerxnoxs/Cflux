# Makefile for the Flux "concurrent" native extension.
#
# Builds libconcurrent.so, which the Flux VM dlopen()s when a script does
# `import concurrent` (see include/flux/extension.h for the plugin ABI).
#
# Usage:
#   make -C extension/concurrent
# or from the project root:
#   make extensions
#
# Requires: pthreads (libc on Linux — no extra package needed).

CC           ?= gcc
FLUX_INCLUDE ?= ../../include

CFLAGS := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
          -fPIC -I$(FLUX_INCLUDE) -I../.. -g -O0 -U_FORTIFY_SOURCE -Wno-cpp

# pthread is part of glibc on Linux; -pthread sets the right flags and links it
LDFLAGS := -shared -pthread

TARGET := libconcurrent.so

SRCS := concurrent_ext.c thread_pool.c transport.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

%.o: %.c concurrent.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
