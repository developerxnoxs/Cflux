# Makefile for the Flux "http" native extension.
#
# Builds libhttp.so (HTTP/1.1 client + server, pure POSIX socket — no libcurl).
# di-dlopen VM saat skrip melakukan `import http`.

CC           ?= gcc
FLUX_INCLUDE ?= ../../include

CFLAGS  := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
           -fPIC -I$(FLUX_INCLUDE) -g -O2 -U_FORTIFY_SOURCE -Wno-cpp
LDFLAGS := -shared -lssl -lcrypto

TARGET := libhttp.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): http_ext.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
