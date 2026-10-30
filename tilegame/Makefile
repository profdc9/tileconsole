TILEGFX ?= ../tilegfx
TILESND ?= ../tilesnd
GAMESRC = game/game.c game/level.c game/view.c
FONTSRC = $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii8.c
CFLAGS ?= -std=c11 -Wall -Wextra -O2
INC = -Iinclude -Isdl -I$(TILEGFX) -I$(TILESND)
SDL = $(shell pkg-config --cflags --libs sdl2)

all: game_demo platformer test_platform_sdl test_hid_kbd test_hid_pad test_game test_view

test_platform_sdl: tests/test_platform_sdl.c sdl/plat_sdl.c sdl/plat_sdl.h include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_platform_sdl.c sdl/plat_sdl.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

game_demo: demo/game_demo.c sdl/plat_sdl.c include/platform.h $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) demo/game_demo.c sdl/plat_sdl.c $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c -o $@ $(SDL) -lm

test_hid_kbd: tests/test_hid_kbd.c src/hid_kbd.c include/hid_kbd.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_kbd.c src/hid_kbd.c -o $@

test_game: tests/test_game.c game/game.c game/level.c game/game.h game/level.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Igame tests/test_game.c game/game.c game/level.c -o $@

platformer: game/game_main.c $(GAMESRC) sdl/plat_sdl.c include/platform.h $(TILEGFX)/tilegfx.c $(FONTSRC) $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) -Igame game/game_main.c $(GAMESRC) sdl/plat_sdl.c $(TILEGFX)/tilegfx.c $(FONTSRC) $(TILESND)/snd.c -o $@ $(SDL) -lm

test_view: tests/test_view.c $(GAMESRC) game/game.h game/level.h game/view.h $(TILEGFX)/tilegfx.c $(FONTSRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Igame tests/test_view.c $(GAMESRC) $(TILEGFX)/tilegfx.c $(FONTSRC) -o $@ -lm

test_hid_pad: tests/test_hid_pad.c src/hid_pad.c src/hid_mouse.c include/hid_pad.h include/hid_mouse.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_pad.c src/hid_pad.c src/hid_mouse.c -o $@

check: test_platform_sdl test_hid_kbd test_hid_pad test_game test_view
	./test_platform_sdl
	./test_hid_kbd
	./test_hid_pad
	./test_game
	./test_view

clean:
	rm -f game_demo platformer test_platform_sdl test_hid_kbd test_hid_pad test_game test_view
