TILEGFX ?= ../tilegfx
TILESND ?= ../tilesnd
CFLAGS ?= -std=c11 -Wall -Wextra -O2
INC = -Iinclude -Isdl -I$(TILEGFX) -I$(TILESND)
SDL = $(shell pkg-config --cflags --libs sdl2)

all: game_demo test_platform_sdl test_hid_kbd

test_platform_sdl: tests/test_platform_sdl.c sdl/plat_sdl.c sdl/plat_sdl.h include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_platform_sdl.c sdl/plat_sdl.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

game_demo: demo/game_demo.c sdl/plat_sdl.c include/platform.h $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) demo/game_demo.c sdl/plat_sdl.c $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c -o $@ $(SDL) -lm

test_hid_kbd: tests/test_hid_kbd.c src/hid_kbd.c include/hid_kbd.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_kbd.c src/hid_kbd.c -o $@

check: test_platform_sdl test_hid_kbd
	./test_platform_sdl
	./test_hid_kbd

clean:
	rm -f game_demo test_platform_sdl test_hid_kbd
