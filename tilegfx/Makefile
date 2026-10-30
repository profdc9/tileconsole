CFLAGS ?= -std=c11 -Wall -Wextra -O2

all: test_tilegfx demo_sdl

test_tilegfx: test_tilegfx.c tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined test_tilegfx.c tilegfx.c -o $@

demo_sdl: demo_sdl.c tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) demo_sdl.c tilegfx.c -o $@ $(shell pkg-config --cflags --libs sdl2) -lm

check: test_tilegfx
	./test_tilegfx

clean:
	rm -f test_tilegfx demo_sdl shot.ppm shot.png
