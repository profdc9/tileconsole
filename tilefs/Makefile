CFLAGS ?= -std=c11 -Wall -Wextra -O2
SRC = tilefs.c fs_ram.c fs_posix.c

test_tilefs: test_tilefs.c $(SRC) tilefs.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined test_tilefs.c $(SRC) -o $@

check: test_tilefs
	./test_tilefs

clean:
	rm -f test_tilefs *.o
