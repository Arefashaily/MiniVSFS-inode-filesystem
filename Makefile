CC ?= gcc
CFLAGS ?= -O2 -std=c17 -Wall -Wextra
PYTHON ?= python3

all: mkfs_builder mkfs_adder

mkfs_builder: src/mkfs_builder.c src/minivsfs.h
	$(CC) $(CFLAGS) $< -o $@

mkfs_adder: src/mkfs_adder.c src/minivsfs.h
	$(CC) $(CFLAGS) $< -o $@

test: all
	$(PYTHON) tests/run_tests.py .

clean:
	rm -f mkfs_builder mkfs_adder *.img

.PHONY: all test clean
