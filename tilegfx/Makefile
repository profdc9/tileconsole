CFLAGS ?= -std=c11 -Wall -Wextra -O2

all: test_tilegfx test_world test_actor test_font test_text test_console demo_sdl demo_text

test_tilegfx: test_tilegfx.c tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined test_tilegfx.c tilegfx.c -o $@

demo_sdl: demo_sdl.c scene.c scene.h tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) demo_sdl.c scene.c tilegfx.c -o $@ $(shell pkg-config --cflags --libs sdl2) -lm

test_world: test_world.c tg_world.c tg_world.h tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_world.c tg_world.c tilegfx.c -o $@

test_actor: test_actor.c tg_actor.c tg_actor.h tg_world.c tg_world.h tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_actor.c tg_actor.c tg_world.c tilegfx.c -o $@

test_font: test_font.c tg_font.c tg_fontlist.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h $(wildcard fonts/*.c)
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_font.c tg_font.c tg_fontlist.c tilegfx.c fonts/*.c -o $@

demo_text: demo_text.c tg_font.c tg_fontlist.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h $(wildcard fonts/*.c)
	$(CC) $(CFLAGS) -I. demo_text.c tg_font.c tg_fontlist.c tilegfx.c fonts/*.c -o $@

test_text: test_text.c tg_text.c tg_text_cp437.c tg_text.h tg_font.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h fonts/unscii16.c
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_text.c tg_text.c tg_text_cp437.c tg_font.c tilegfx.c fonts/unscii16.c -o $@

test_console: test_console.c tg_console.c tg_console.h tg_text.c tg_text.h tilegfx.c tilegfx.h
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_console.c tg_console.c tg_text.c tilegfx.c -o $@

# The console's ANSI mode against Daniel Marks' TNTSCAnsi (PicoParanoia, zlib licence). Needs that project: make test_ansi_oracle PICOPARANOIA=~/projects/PicoParanoia
PICOPARANOIA ?= $(HOME)/projects/PicoParanoia
test_ansi_oracle: test_ansi_oracle.cpp tg_console.c tg_console.h tg_text.c tilegfx.c
	@test -f $(PICOPARANOIA)/Code/src/TNTSCAnsi.cpp || { echo "no $(PICOPARANOIA)/Code/src/TNTSCAnsi.cpp: set PICOPARANOIA"; exit 1; }
	mkdir -p oracle_build
	cp $(PICOPARANOIA)/Code/src/TNTSCAnsi.cpp $(PICOPARANOIA)/Code/src/TNTSCAnsi.h oracle_build/
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined -c tg_console.c -o oracle_build/tg_console.o
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined -c tg_text.c -o oracle_build/tg_text.o
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined -c tilegfx.c -o oracle_build/tilegfx.o
	$(CXX) -std=c++11 -O2 -g -fpermissive -w -Ioracle_build -I. -fsanitize=address,undefined test_ansi_oracle.cpp oracle_build/TNTSCAnsi.cpp oracle_build/tg_console.o oracle_build/tg_text.o oracle_build/tilegfx.o -o $@

test_fontmask: test_fontmask.c tg_font.c tg_fontlist.c tg_font.h tg_fonts.h tilegfx.c tilegfx.h fonts/unscii8.c fonts/unscii8_thin.c
	$(CC) $(CFLAGS) -I. -DTG_FONTS_ENABLED=0x5 test_fontmask.c tg_font.c tg_fontlist.c tilegfx.c fonts/unscii8.c fonts/unscii8_thin.c -o $@ -Wl,--gc-sections -ffunction-sections -fdata-sections

check: test_tilegfx test_world test_actor test_font test_text test_console test_fontmask
	./test_tilegfx
	./test_world
	./test_actor
	./test_font
	./test_text
	./test_console
	./test_fontmask

clean:
	rm -rf oracle_build test_ansi_oracle; rm -f test_tilegfx test_world test_actor test_font test_text test_console test_fontmask demo_sdl demo_text fonts_extras.png *.ppm shot.png
