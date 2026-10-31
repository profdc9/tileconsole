TILEGFX ?= ../tilegfx
TILESND ?= ../tilesnd
GAMESRC = game/game.c game/level.c game/view.c
FONTSRC = $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii8.c
CFLAGS ?= -std=c11 -Wall -Wextra -O2
INC = -Iinclude -Isdl -I$(TILEGFX) -I$(TILESND)
SDL = $(shell pkg-config --cflags --libs sdl2)

all: game_demo padtest platformer textdemo test_platform_sdl test_hid_kbd test_hid_pad test_game test_view

test_platform_sdl: tests/test_platform_sdl.c sdl/plat_sdl.c src/mouse_acc.c sdl/plat_sdl.h include/platform.h $(TILEGFX)/tilegfx.c $(TILEGFX)/tg_text.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_platform_sdl.c sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(TILEGFX)/tg_text.c -o $@ $(SDL) -lm

game_demo: demo/game_demo.c sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) demo/game_demo.c sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c -o $@ $(SDL) -lm

test_hid_kbd: tests/test_hid_kbd.c src/hid_kbd.c include/hid_kbd.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_kbd.c src/hid_kbd.c -o $@

test_game: tests/test_game.c game/game.c game/level.c game/game.h game/level.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Igame tests/test_game.c game/game.c game/level.c -o $@

platformer: game/game_main.c $(GAMESRC) sdl/plat_sdl.c include/platform.h $(TILEGFX)/tilegfx.c $(FONTSRC) $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) -Igame game/game_main.c $(GAMESRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(FONTSRC) $(TILESND)/snd.c -o $@ $(SDL) -lm

padtest: demo/padtest.c sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c $(FONTSRC)
	$(CC) $(CFLAGS) $(INC) demo/padtest.c sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(FONTSRC) -o $@ $(SDL) -lm

TEXTSRC = $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c demo/textdemo_pages.c

textdemo: demo/textdemo.c $(TEXTSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) $(INC) -Idemo demo/textdemo.c $(TEXTSRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

shot_text: tests/shot_text.c $(TEXTSRC) $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Idemo tests/shot_text.c $(TEXTSRC) $(TILEGFX)/tilegfx.c -o $@

test_view: tests/test_view.c $(GAMESRC) game/game.h game/level.h game/view.h $(TILEGFX)/tilegfx.c $(FONTSRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Igame tests/test_view.c $(GAMESRC) $(TILEGFX)/tilegfx.c $(FONTSRC) -o $@ -lm

test_hid_pad: tests/test_hid_pad.c src/hid_pad.c src/hid_mouse.c src/mouse_acc.c src/xinput.c include/mouse_acc.h include/xinput.h include/hid_pad.h include/hid_mouse.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_pad.c src/hid_pad.c src/hid_mouse.c src/mouse_acc.c src/xinput.c -o $@

check: test_platform_sdl test_hid_kbd test_hid_pad test_game test_view
	./test_platform_sdl
	./test_hid_kbd
	./test_hid_pad
	./test_game
	./test_view

clean:
	rm -f game_demo padtest platformer test_platform_sdl test_hid_kbd test_hid_pad test_game test_view
