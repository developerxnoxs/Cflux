CC            ?= gcc
FLUX_INCLUDE  ?= ../../include
CFLAGS        := -std=gnu17 -Wall -Wextra -Wno-unused-parameter \
                 -fPIC -I$(FLUX_INCLUDE) -g -O0

all: libaio.so

libaio.so: async_ext.c
	$(CC) $(CFLAGS) -shared -o $@ $< -luv -lm

clean:
	rm -f libaio.so libasync.so
