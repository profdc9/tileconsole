TILEGFX ?= ../tilegfx
TILESND ?= ../tilesnd
GAMESRC = game/game.c game/level.c game/view.c
FONTSRC = $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii8.c
CFLAGS ?= -std=c11 -Wall -Wextra -O2
INC = -Iinclude -Isdl -I$(TILEGFX) -I$(TILESND)
SDL = $(shell pkg-config --cflags --libs sdl2)

all: editsd test_editsd sdtest test_sdtest game_demo padtest platformer textdemo consdemo editdemo test_editdemo test_consdemo test_consoleio test_platform_sdl test_hid_kbd test_keymap test_hid_pad test_game test_view

test_platform_sdl: tests/test_platform_sdl.c sdl/plat_sdl.c src/mouse_acc.c sdl/plat_sdl.h include/platform.h $(TILEGFX)/tilegfx.c $(TILEGFX)/tg_text.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_platform_sdl.c sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(TILEGFX)/tg_text.c -o $@ $(SDL) -lm

game_demo: demo/game_demo.c sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) demo/game_demo.c sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(TILEGFX)/scene.c $(TILESND)/snd.c -o $@ $(SDL) -lm

test_hid_kbd: tests/test_hid_kbd.c src/hid_kbd.c include/hid_kbd.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_kbd.c src/hid_kbd.c -o $@

test_keymap: tests/test_keymap.c src/keymap.c include/keymap.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_keymap.c src/keymap.c -o $@

test_game: tests/test_game.c game/game.c game/level.c game/game.h game/level.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Igame tests/test_game.c game/game.c game/level.c -o $@

platformer: game/game_main.c $(GAMESRC) sdl/plat_sdl.c include/platform.h $(TILEGFX)/tilegfx.c $(FONTSRC) $(TILESND)/snd.c
	$(CC) $(CFLAGS) $(INC) -Igame game/game_main.c $(GAMESRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(FONTSRC) $(TILESND)/snd.c -o $@ $(SDL) -lm

padtest: demo/padtest.c sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c $(FONTSRC)
	$(CC) $(CFLAGS) $(INC) demo/padtest.c sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c $(FONTSRC) -o $@ $(SDL) -lm

TEXTSRC = $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c demo/textdemo_pages.c

textdemo: demo/textdemo.c $(TEXTSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) $(INC) -Idemo demo/textdemo.c $(TEXTSRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

CONSSRC = $(TILEGFX)/tg_console.c $(TEXTSRC) demo/consdemo_logic.c

consdemo: demo/consdemo.c $(CONSSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) $(INC) -Idemo demo/consdemo.c $(CONSSRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

test_consdemo: tests/test_consdemo.c $(CONSSRC) demo/consdemo_logic.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Idemo tests/test_consdemo.c $(CONSSRC) $(TILEGFX)/tilegfx.c -o $@

CONSOLEIO_SRC = src/console_shim.c $(TILEGFX)/tg_console.c $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c $(TILEGFX)/tilegfx.c

test_consoleio: tests/test_consoleio.c $(CONSOLEIO_SRC) include/consoleio.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_consoleio.c $(CONSOLEIO_SRC) -o $@

test_editor: tests/test_editor.c editor/editor.c editor/editor.h $(CONSOLEIO_SRC) include/consoleio.h
	$(CC) $(CFLAGS) -Wno-old-style-definition -g -fsanitize=address,undefined $(INC) -Ieditor tests/test_editor.c editor/editor.c $(CONSOLEIO_SRC) -o $@

# The editor against PicoParanoia's own: random sessions through both, compared line by line. Needs that project: make test_editor_oracle PICOPARANOIA=~/projects/PicoParanoia
PICOPARANOIA ?= $(HOME)/projects/PicoParanoia
ORACLE = oracle_build
SESSIONS ?= 3000
test_editor_oracle: tests/editor_oracle.c tests/editor_oracle_tnt.cpp editor/editor.c $(CONSOLEIO_SRC) include/consoleio.h
	@test -f $(PICOPARANOIA)/Code/src/editor.c || { echo "no $(PICOPARANOIA)/Code/src/editor.c: set PICOPARANOIA"; exit 1; }
	mkdir -p $(ORACLE)
	cp $(PICOPARANOIA)/Code/src/editor.c $(PICOPARANOIA)/Code/src/editor.h $(PICOPARANOIA)/Code/src/consoleio.h $(PICOPARANOIA)/Code/src/TNTSCAnsi.cpp $(PICOPARANOIA)/Code/src/TNTSCAnsi.h $(ORACLE)/
	@# The copy gets the fixes that make the comparison defined (show_gets leaves a prompt string unterminated when all that was typed is erased; goto_ptr is one line short when the place is a line break; the macro buffer has no end; any right margin is taken; a mark outside the text is used): the same as in editor/editor.c.
	sed -i 's/if(col > 0) buf\[col\] = 0;/if(col >= 0) buf[col] = 0;/' $(ORACLE)/editor.c
	sed -i 's/while(ewb > s) cursor_up();/while(ewb >= s \&\& ytru > 0) cursor_up();/' $(ORACLE)/editor.c
	sed -i 's/    \*s++ = k;/    if(s < mbuf+sizeof(mbuf)-1) *s++ = k;/' $(ORACLE)/editor.c
	sed -i 's/^  ae = temp;/  ae = temp; if(mk > ae) mk = ae;/' $(ORACLE)/editor.c
	sed -i 's/^  if (mk > dp) {/  if (mk > ae || mk < aa+1) { show_note("Invalid Pos.",2); return; }\n  if (mk > dp) {/' $(ORACLE)/editor.c
	sed -i 's/    sww = mystrtol(wbuf,NULL);/    sww = mystrtol(wbuf,NULL); if(sww < XINC) sww = XINC; if(sww > 78) sww = 78;/' $(ORACLE)/editor.c
	@grep -q 'col >= 0) buf' $(ORACLE)/editor.c && grep -q 'ewb >= s && ytru' $(ORACLE)/editor.c && grep -q 's < mbuf+sizeof' $(ORACLE)/editor.c && grep -q 'if(sww > 78)' $(ORACLE)/editor.c && grep -q 'if(mk > ae) mk = ae;' $(ORACLE)/editor.c && grep -q 'mk < aa+1' $(ORACLE)/editor.c || { echo "the patches of the copy of editor.c did not apply"; exit 1; }
	$(CXX) -std=c++11 -O2 -g -fpermissive -w -I$(ORACLE) -fsanitize=address,undefined -c tests/editor_oracle_tnt.cpp -o $(ORACLE)/tnt.o
	$(CXX) -std=c++11 -O2 -g -fpermissive -w -I$(ORACLE) -fsanitize=address,undefined -c $(ORACLE)/TNTSCAnsi.cpp -o $(ORACLE)/TNTSCAnsi.o
	$(CC) -std=gnu11 -O2 -g -w -DTHEIRS -I$(ORACLE) -Iinclude -I$(TILEGFX) -fsanitize=address,undefined -c tests/editor_oracle.c -o $(ORACLE)/theirs.o
	$(CXX) -fsanitize=address,undefined $(ORACLE)/theirs.o $(ORACLE)/tnt.o $(ORACLE)/TNTSCAnsi.o -o $(ORACLE)/theirs
	$(CC) -std=gnu11 -O2 -g -w -Ieditor -Iinclude -I$(TILEGFX) -fsanitize=address,undefined tests/editor_oracle.c editor/editor.c $(CONSOLEIO_SRC) -o $(ORACLE)/ours
	$(ORACLE)/theirs $(SESSIONS) > $(ORACLE)/theirs.txt
	$(ORACLE)/ours $(SESSIONS) > $(ORACLE)/ours.txt
	@paste -d'|' $(ORACLE)/theirs.txt $(ORACLE)/ours.txt | awk -F'|' '{ t = $$1; o = $$2; tc = (t ~ /CRASH/); oc = (o ~ /CRASH/); \
	  if (t == o) { if (tc) both_crash++; else same++ } \
	  else if (tc && !oc) original_only++; \
	  else { bad++; if (bad <= 5) print "DIFFERENT: theirs: " t "\n           ours:   " o } } \
	  END { printf "%d sessions: %d the same, %d crash in both, %d crash in PicoParanoia'"'"'s editor only, %d different\n", NR, same, both_crash, original_only, bad; exit bad > 0 }' \
	  || { echo "MISMATCH"; exit 1; }
	@echo "(a crash is a session in which that build stopped on a memory error: the original has some, see the notice at the top of editor/editor.c)"

EDITSRC = demo/editdemo_logic.c editor/editor.c $(CONSOLEIO_SRC) src/console_platform.c

editdemo: demo/editdemo.c $(EDITSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h
	$(CC) $(CFLAGS) -Wno-old-style-definition $(INC) -Ieditor -Idemo demo/editdemo.c $(EDITSRC) sdl/plat_sdl.c src/mouse_acc.c -o $@ $(SDL) -lm

test_editdemo: tests/test_editdemo.c demo/editdemo_logic.c demo/editdemo_logic.h editor/editor.c $(CONSOLEIO_SRC)
	$(CC) $(CFLAGS) -Wno-old-style-definition -g -fsanitize=address,undefined $(INC) -Ieditor -Idemo tests/test_editdemo.c demo/editdemo_logic.c editor/editor.c $(CONSOLEIO_SRC) -o $@

shot_cons: tests/shot_cons.c $(CONSSRC) $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Idemo tests/shot_cons.c $(CONSSRC) $(TILEGFX)/tilegfx.c -o $@

shot_text: tests/shot_text.c $(TEXTSRC) $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Idemo tests/shot_text.c $(TEXTSRC) $(TILEGFX)/tilegfx.c -o $@

test_view: tests/test_view.c $(GAMESRC) game/game.h game/level.h game/view.h $(TILEGFX)/tilegfx.c $(FONTSRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) -Igame tests/test_view.c $(GAMESRC) $(TILEGFX)/tilegfx.c $(FONTSRC) -o $@ -lm

test_hid_pad: tests/test_hid_pad.c src/hid_pad.c src/hid_mouse.c src/mouse_acc.c src/xinput.c include/mouse_acc.h include/xinput.h include/hid_pad.h include/hid_mouse.h include/platform.h
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) tests/test_hid_pad.c src/hid_pad.c src/hid_mouse.c src/mouse_acc.c src/xinput.c -o $@

check: test_editsd test_sdtest test_platform_sdl test_hid_kbd test_keymap test_consdemo test_consoleio test_editor test_editdemo test_hid_pad test_game test_view
	./test_platform_sdl
	./test_hid_kbd
	./test_keymap
	./test_sdtest
	./test_editsd
	./test_consdemo
	./test_consoleio
	./test_editor
	./test_editdemo
	./test_hid_pad
	./test_game
	./test_view

clean:
	rm -rf oracle_build
	rm -f game_demo padtest platformer test_platform_sdl test_hid_kbd test_hid_pad test_game test_view

# The SD card test, with the file system layer (tilefs) beside this directory.
TILEFS = ../tilefs
FATFS = $(TILEFS)/third_party/fatfs
TILEFSSRC = $(TILEFS)/tilefs.c $(TILEFS)/fs_ram.c $(TILEFS)/fs_posix.c $(TILEFS)/fs_blk.c $(TILEFS)/fs_blk_file.c $(TILEFS)/fs_fat.c $(TILEFS)/fs_fat_diskio.c $(FATFS)/ff.c $(FATFS)/ffunicode.c
SDINC = -I$(TILEFS) -I$(FATFS) -Idemo
SDTESTSRC = demo/sdtest_logic.c $(TILEGFX)/tg_console.c $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c $(TILEFSSRC)

sdtest: demo/sdtest.c $(SDTESTSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) $(INC) $(SDINC) demo/sdtest.c $(SDTESTSRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

test_sdtest: tests/test_sdtest.c $(SDTESTSRC) demo/sdtest_logic.h $(TILEGFX)/tilegfx.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $(INC) $(SDINC) tests/test_sdtest.c $(SDTESTSRC) $(TILEGFX)/tilegfx.c -o $@

# The file editor demo: the editor on files, through tilefs.
EDITSDSRC = demo/editsd_logic.c editor/editor.c $(CONSOLEIO_SRC) src/console_platform.c $(TILEFSSRC)

editsd: demo/editsd.c $(EDITSDSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h
	$(CC) $(CFLAGS) -Wno-old-style-definition $(INC) $(SDINC) -Ieditor demo/editsd.c $(EDITSDSRC) sdl/plat_sdl.c src/mouse_acc.c -o $@ $(SDL) -lm

test_editsd: tests/test_editsd.c demo/editsd_logic.c demo/editsd_logic.h editor/editor.c $(CONSOLEIO_SRC) $(TILEFSSRC)
	$(CC) $(CFLAGS) -Wno-old-style-definition -g -fsanitize=address,undefined $(INC) $(SDINC) -Ieditor tests/test_editsd.c demo/editsd_logic.c editor/editor.c $(CONSOLEIO_SRC) $(TILEFSSRC) -o $@
