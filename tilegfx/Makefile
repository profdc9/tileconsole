CFLAGS ?= -std=c11 -Wall -Wextra -O2

all: test_tilegfx test_font demo_sdl demo_text

test_tilegfx: test_tilegfx.c tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined test_tilegfx.c tilegfx.c -o $@

demo_sdl: demo_sdl.c scene.c scene.h tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) demo_sdl.c scene.c tilegfx.c -o $@ $(shell pkg-config --cflags --libs sdl2) -lm

test_font: test_font.c tg_font.c tg_fontlist.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h $(wildcard fonts/*.c)
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_font.c tg_font.c tg_fontlist.c tilegfx.c fonts/*.c -o $@

demo_text: demo_text.c tg_font.c tg_fontlist.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h $(wildcard fonts/*.c)
	$(CC) $(CFLAGS) -I. demo_text.c tg_font.c tg_fontlist.c tilegfx.c fonts/*.c -o $@

test_fontmask: test_fontmask.c tg_font.c tg_fontlist.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h fonts/unscii8.c fonts/unscii8_thin.c
	$(CC) $(CFLAGS) -I. -DTG_FONTS_ENABLED=0x5 test_fontmask.c tg_font.c tg_fontlist.c tilegfx.c fonts/unscii8.c fonts/unscii8_thin.c -o $@ -Wl,--gc-sections -ffunction-sections -fdata-sections

check: test_tilegfx test_font test_fontmask
	./test_tilegfx
	./test_font
	./test_fontmask

clean:
	rm -f test_tilegfx test_font test_fontmask demo_sdl demo_text fonts_extras.png *.ppm shot.png
