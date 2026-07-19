# Makefile for the Flux "mysql" native extension.
#
# Builds libmysql.so, yang di-dlopen VM saat skrip melakukan `import mysql`.
#
# Usage:
#   make -C extension/mysql
# atau dari root proyek:
#   make extensions

CC           ?= gcc
FLUX_INCLUDE ?= ../../include

# Path ke header dan library MariaDB Connector/C dari nix store
MYSQL_INC    := /nix/store/437dc0c362ixi9nn4v76b6rzfygbd7k2-mariadb-connector-c-3.3.5-dev/include/mysql
MYSQL_LIB    := /nix/store/bz46zm1kpqn08rhhh3hrd9r5f74kbx6k-mariadb-connector-c-3.3.5/lib/mysql

CFLAGS  := -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
           -fPIC -I$(FLUX_INCLUDE) -I$(MYSQL_INC) -g -O0 -U_FORTIFY_SOURCE -Wno-cpp
LDFLAGS := -L$(MYSQL_LIB) -lmysqlclient -shared -Wl,-rpath,$(MYSQL_LIB)

TARGET := libmysql.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): mysql_ext.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
