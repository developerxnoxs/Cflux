# Makefile for the Flux "ws" native extension.
#
# Builds libws.so — WebSocket server berbasis wslay + raw POSIX socket.
# di-dlopen VM saat skrip melakukan `import ws`.

CC           ?= gcc
FLUX_INCLUDE ?= ../../include

CFLAGS  := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
           -fPIC -I$(FLUX_INCLUDE) -g -O2

LDFLAGS := -shared -lwslay -lssl -lcrypto

TARGET := libws.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): ws_ext.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
