CC            ?= gcc
FLUX_INCLUDE  ?= ../../include
CFLAGS        := -std=gnu17 -Wall -Wextra -Wno-unused-parameter \
                 -fPIC -I$(FLUX_INCLUDE) -g -O0 -U_FORTIFY_SOURCE -Wno-cpp

all: libasync.so

libasync.so: async_ext.c
	$(CC) $(CFLAGS) -shared -o $@ $< -luv -lm

clean:
	rm -f libasync.so
