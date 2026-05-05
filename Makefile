CC ?= cc
MODULE = rtree.so

SRC = src/module.c src/rtree.c src/art.c src/rdb.c
OBJ = $(SRC:.c=.o)

CPPFLAGS ?= -Ideps -Isrc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -fPIC -fcommon

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SO_LDFLAGS = -bundle -undefined dynamic_lookup
else
  SO_LDFLAGS = -shared
endif

.PHONY: all test test-art test-module clean

all: $(MODULE)

$(MODULE): $(OBJ)
	$(CC) $(SO_LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c src/*.h deps/redismodule.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/test_art: tests/test_art.c src/art.c src/art.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_art.c src/art.c

test-art: tests/test_art
	./tests/test_art

test-module: $(MODULE)
	ruby tests/test_module.rb

test: test-art

clean:
	rm -f $(OBJ) $(MODULE) tests/test_art
