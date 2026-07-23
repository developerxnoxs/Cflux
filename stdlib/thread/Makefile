# Builds stdlib/thread/libthread.so — modul thread pool untuk Flux.
CC           ?= gcc
FLUX_INCLUDE ?= ../../include
CFLAGS       := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
                -fPIC -I$(FLUX_INCLUDE) -g -O0 -U_FORTIFY_SOURCE -Wno-cpp
LDFLAGS      := -luv -lpthread

TARGET := libthread.so
SRC    := thread_module.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -shared -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
