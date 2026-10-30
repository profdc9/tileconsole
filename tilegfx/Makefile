CFLAGS ?= -std=c11 -Wall -Wextra -O2

all: test_tilegfx test_font demo_sdl demo_text

test_tilegfx: test_tilegfx.c tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined test_tilegfx.c tilegfx.c -o $@

demo_sdl: demo_sdl.c scene.c scene.h tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) demo_sdl.c scene.c tilegfx.c -o $@ $(shell pkg-config --cflags --libs sdl2) -lm

test_font: test_font.c tg_font.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h $(wildcard fonts/*.c)
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_font.c tg_font.c tilegfx.c fonts/*.c -o $@

demo_text: demo_text.c tg_font.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h $(wildcard fonts/*.c)
	$(CC) $(CFLAGS) -I. demo_text.c tg_font.c tilegfx.c fonts/*.c -o $@

check: test_tilegfx test_font
	./test_tilegfx
	./test_font

clean:
	rm -f test_tilegfx test_font demo_sdl demo_text fonts_extras.png *.ppm shot.png
