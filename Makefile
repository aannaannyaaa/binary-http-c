CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE

ifeq ($(OS),Windows_NT)
  CC     = gcc
  EXE    = .exe
  LDLIBS = -lws2_32
else
  LDLIBS = -lpthread
endif

all: bserve$(EXE) bcurl$(EXE)

bserve$(EXE): src/bserve.c src/bfp.c src/bfp.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/bfp.c $(LDLIBS)

bcurl$(EXE): src/bcurl.c src/bfp.c src/bfp.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/bfp.c $(LDLIBS)

test: all
	python3 tests/test_bfp.py

clean:
	rm -f bserve bcurl bserve.exe bcurl.exe

.PHONY: all test clean
