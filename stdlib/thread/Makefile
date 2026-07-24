# Builds stdlib/thread/libthread.so — modul thread pool untuk Flux.
CC           ?= gcc
FLUX_INCLUDE ?= ../../include
CFLAGS       := -std=gnu17 -D_GNU_SOURCE -DCONCURRENT_NO_EXTENSION_INIT \
                -Wall -Wextra -Wno-unused-parameter \
                -fPIC -I$(FLUX_INCLUDE) -I../.. -g -O0 -U_FORTIFY_SOURCE -Wno-cpp
LDFLAGS      := -luv -lpthread

TARGET := libthread.so
BUILD  := .build
OBJS   := $(BUILD)/thread_module.o $(BUILD)/concurrent_ext.o \
          $(BUILD)/thread_pool.o $(BUILD)/transport.o

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -shared -o $@ $(LDFLAGS)

$(BUILD):
	mkdir -p $@

$(BUILD)/thread_module.o: thread_module.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/concurrent_ext.o: ../../extension/concurrent/concurrent_ext.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/thread_pool.o: ../../extension/concurrent/thread_pool.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/transport.o: ../../extension/concurrent/transport.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(TARGET) $(BUILD)
