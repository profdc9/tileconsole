TILEGFX ?= ../tilegfx
TILESND ?= ../tilesnd
GAMESRC = game/game.c game/level.c game/view.c
FONTSRC = $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii8.c
CFLAGS ?= -std=c11 -Wall -Wextra -O2
INC = -Iinclude -Isdl -I$(TILEGFX) -I$(TILESND)
SDL = $(shell pkg-config --cflags --libs sdl2)

all: devkit tgbasic test_basic editsd test_editsd sdtest test_sdtest game_demo padtest platformer textdemo consdemo editdemo test_editdemo test_consdemo test_consoleio test_platform_sdl test_hid_kbd test_keymap test_hid_pad test_game test_view

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

check: test_usblog test_pool test_devkit test_basic test_tde test_tde_san test_editsd test_sdtest test_platform_sdl test_hid_kbd test_keymap test_consdemo test_consoleio test_editor test_editdemo test_hid_pad test_game test_view
	./test_usblog
	./test_pool
	./test_platform_sdl
	./test_hid_kbd
	./test_keymap
	./test_sdtest
	./test_editsd
	./test_basic
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test_devkit
	./test_tde
	ASAN_OPTIONS=detect_leaks=0 ./test_tde_san
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

# TDE, the Thomson-Davis Editor (public domain), built like its Unix version with the tilegame console and files (tde/os) instead of curses.
TDESRC = $(filter-out tde/src/main.c tde/src/menucua.c,$(wildcard tde/src/*.c))   # (menucua.c is the alternative menu, included by menu.c when asked for)
TDEOBJ = $(patsubst tde/src/%.c,build/tde/%.o,$(TDESRC)) build/tde/main.o build/tde/console.o build/tde/port.o build/tde/criterr.o build/tde/tde_libc.o
TDECFLAGS = -std=gnu11 -O2 -w -U_FORTIFY_SOURCE -D__UNIX__ -D__TILEGAME__ -Itde/src -Itde/os -Iinclude -I$(TILEGFX) -I$(TILEFS) -I$(FATFS)
include $(TILEFS)/../tilegame/tde/os/Makefile.frag

build/tde/%.o: tde/src/%.c
	@mkdir -p build/tde
	$(CC) -c $(TDECFLAGS) $< -o $@
build/tde/main.o: tde/src/main.c
	@mkdir -p build/tde
	$(CC) -c $(TDECFLAGS) -Dmain=tde_main $< -o $@
build/tde/%.o: tde/os/%.c
	@mkdir -p build/tde
	$(CC) -c $(TDECFLAGS) -I$(TILEGFX) $< -o $@

TDEFSSRC = $(filter-out $(TILEFS)/fs_posix.c,$(TILEFSSRC))   # (the host backend is built apart: it must reach the real C library, not the editor's)
TDEPLAT = tde/tdeapp.c sdl/plat_sdl.c src/mouse_acc.c src/keymap.c $(TILEGFX)/tilegfx.c $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c $(TDEFSSRC) build/tde/fs_posix.o

build/tde/fs_posix.o: $(TILEFS)/fs_posix.c
	@mkdir -p build/tde
	$(CC) -c -std=gnu11 -O2 -w -DTILEFS_WRAPPED_LIBC -I$(TILEFS) $< -o $@

# The program is built as the editor's own makefile builds it, with -DNDEBUG (no assert checks: some of them are for the author's debugging and would end the editor
# for a harmless reason); the tests use the same sources with the checks on, so that a violated assumption is found.
TDERELOBJ = $(patsubst build/tde/%.o,build/tder/%.o,$(TDEOBJ))
build/tder/%.o: tde/src/%.c
	@mkdir -p build/tder
	$(CC) -c $(TDECFLAGS) -DNDEBUG $< -o $@
build/tder/main.o: tde/src/main.c
	@mkdir -p build/tder
	$(CC) -c $(TDECFLAGS) -DNDEBUG -Dmain=tde_main $< -o $@
build/tder/%.o: tde/os/%.c
	@mkdir -p build/tder
	$(CC) -c $(TDECFLAGS) -DNDEBUG -I$(TILEGFX) $< -o $@

tde_ed: $(TDERELOBJ) $(TDEPLAT) include/platform.h
	$(CC) $(CFLAGS) -w -U_FORTIFY_SOURCE $(INC) $(SDINC) -Itde/os $(TDERELOBJ) $(TDEPLAT) $(TDE_WRAP) -o $@ $(SDL) -lm

# The development kit on the desktop: BASIC and the editor in one program (EDIT runs the editor in the same process).
DEVKITBASIC = basic/basic_env.c basic/basic_game.c basic/my_basic/my_basic.c $(TILEGFX)/tg_console.c $(TILEGFX)/tg_world.c $(TILEGFX)/tg_actor.c
devkit: demo/devkit.c $(TDERELOBJ) $(DEVKITBASIC) $(TDEPLAT) include/platform.h build/devkit_basic.o
	$(CC) $(CFLAGS) -w -U_FORTIFY_SOURCE $(INC) $(SDINC) -Itde/os -Ibasic demo/devkit.c $(TDERELOBJ) build/devkit_basic.o $(filter-out tde/tdeapp.c,$(TDEPLAT)) $(TDE_WRAP) -o $@ $(SDL) -lm

test_devkit: tests/test_devkit.c demo/devkit.c $(TDERELOBJ) $(DEVKITBASIC) $(TDEPLAT) include/platform.h build/devkit_basic.o
	$(CC) $(CFLAGS) -w -U_FORTIFY_SOURCE $(INC) $(SDINC) -Itde/os -Ibasic tests/test_devkit.c $(TDERELOBJ) build/devkit_basic.o $(filter-out tde/tdeapp.c,$(TDEPLAT)) $(TDE_WRAP) -o $@ $(SDL) -lm -lpthread

build/devkit_basic.o: $(DEVKITBASIC)
	@mkdir -p build
	$(CC) -c $(BASICFLAGS) -O2 $(INC) $(SDINC) -Ibasic basic/basic_env.c -o build/devkit_basic_env.o
	$(CC) -c $(BASICFLAGS) -O2 $(INC) $(SDINC) -Ibasic basic/my_basic/my_basic.c -o build/devkit_my_basic.o
	$(CC) -c $(BASICFLAGS) -O2 $(INC) $(SDINC) $(TILEGFX)/tg_console.c -o build/devkit_tg_console.o
	$(CC) -c $(BASICFLAGS) -O2 $(INC) $(SDINC) -Ibasic basic/basic_game.c -o build/devkit_basic_game.o
	$(CC) -c $(BASICFLAGS) -O2 $(INC) $(SDINC) $(TILEGFX)/tg_world.c -o build/devkit_tg_world.o
	$(CC) -c $(BASICFLAGS) -O2 $(INC) $(SDINC) $(TILEGFX)/tg_actor.c -o build/devkit_tg_actor.o
	ld -r build/devkit_basic_env.o build/devkit_basic_game.o build/devkit_my_basic.o build/devkit_tg_console.o build/devkit_tg_world.o build/devkit_tg_actor.o -o $@

TDETESTSRC = tests/test_tde.c tests/test_tde_console.c src/keymap.c $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c $(TILEGFX)/tilegfx.c $(TDEFSSRC) build/tde/fs_posix.o

test_tde: $(TDEOBJ) $(TDETESTSRC)
	$(CC) -c -w -g -U_FORTIFY_SOURCE -D__UNIX__ -D__TILEGAME__ -Itde/src -Itde/os $(INC) $(SDINC) tests/test_tde_console.c -o build/tde/test_tde_console.o
	$(CC) $(CFLAGS) -w -g -U_FORTIFY_SOURCE -D__UNIX__ -D__TILEGAME__ $(INC) $(SDINC) -Itde/os -Itde/src -Isrc $(TDEOBJ) build/tde/test_tde_console.o $(filter-out tests/test_tde_console.c,$(TDETESTSRC)) src/pool.c $(TDE_WRAP) -o $@ -lm

# The same tests with the editor built with the compiler's checks (AddressSanitizer, UBSan; not the alignment check, as the editor's allocator makes 16-bit
# values at odd addresses on purpose): for finding what the normal build gets away with.
TDESANFLAGS = -std=gnu11 -O1 -g -w -U_FORTIFY_SOURCE -fsanitize=address,undefined -fno-sanitize=alignment -D__UNIX__ -D__TILEGAME__ -Itde/src -Itde/os -Iinclude -I$(TILEGFX) -I$(TILEFS) -I$(FATFS)
TDESANOBJ = $(patsubst build/tde/%.o,build/tdes/%.o,$(TDEOBJ)) build/tdes/test_tde_console.o
build/tdes/%.o: tde/src/%.c
	@mkdir -p build/tdes
	$(CC) -c $(TDESANFLAGS) $< -o $@
build/tdes/main.o: tde/src/main.c
	@mkdir -p build/tdes
	$(CC) -c $(TDESANFLAGS) -Dmain=tde_main $< -o $@
build/tdes/%.o: tde/os/%.c
	@mkdir -p build/tdes
	$(CC) -c $(TDESANFLAGS) $< -o $@
build/tdes/test_tde_console.o: tests/test_tde_console.c
	@mkdir -p build/tdes
	$(CC) -c $(TDESANFLAGS) $< -o $@

test_tde_san: $(TDESANOBJ) tests/test_tde.c $(filter-out tests/test_tde.c tests/test_tde_console.c,$(TDETESTSRC))
	$(CC) $(TDESANFLAGS) -fsanitize=address,undefined $(SDINC) -Itde/os -Isrc $(TDESANOBJ) $(filter-out tests/test_tde_console.c,$(TDETESTSRC)) $(TDE_WRAP) src/pool.c -o $@ -lm

check-san: test_tde_san
	ASAN_OPTIONS=detect_leaks=0 ./test_tde_san

# The BASIC environment: MY-BASIC (basic/my_basic, MIT, unmodified) behind a command prompt on the text screen, programs as text files on tilefs.
# MY-BASIC puts a size tag in front of every block it allocates; its type is a setting (mb_mem_tag_t), and the default 16 bits would leave every block
# misaligned, which is undefined and on the board's CPU can fault: a word, as large as a pointer, keeps them aligned.
# (MY-BASIC's own default is the compact mode, which packs its structures: members at odd addresses, which GCC handles; the alignment check of the sanitizer is off for that.)
BASICFLAGS = -std=gnu11 -Wall -Wno-unused-function -Wno-unused-result -Wno-address-of-packed-member -Dmb_mem_tag_t=size_t
BASICSRC = basic/basic_env.c basic/basic_game.c basic/my_basic/my_basic.c $(TILEGFX)/tg_world.c $(TILEGFX)/tg_actor.c $(TILEGFX)/tg_console.c $(TILEGFX)/tg_text.c $(TILEGFX)/tg_text_cp437.c $(TILEGFX)/tg_font.c $(TILEGFX)/fonts/unscii16.c $(TILEFSSRC)

tgbasic: basic/basic.c $(BASICSRC) sdl/plat_sdl.c src/mouse_acc.c include/platform.h $(TILEGFX)/tilegfx.c
	$(CC) $(BASICFLAGS) -O2 $(INC) $(SDINC) -Ibasic basic/basic.c $(BASICSRC) sdl/plat_sdl.c src/mouse_acc.c $(TILEGFX)/tilegfx.c -o $@ $(SDL) -lm

test_usblog: tests/test_usblog.c src/usblog.c src/usblog.h
	$(CC) -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -Isrc tests/test_usblog.c src/usblog.c -o $@

test_pool: tests/test_pool.c src/pool.c src/pool.h third_party/dlmalloc/malloc.c
	$(CC) -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -Isrc tests/test_pool.c src/pool.c -o $@

test_basic: tests/test_basic.c $(BASICSRC) basic/basic_env.h $(TILEGFX)/tilegfx.c
	$(CC) $(BASICFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize=alignment $(INC) $(SDINC) -Ibasic tests/test_basic.c $(BASICSRC) $(TILEGFX)/tilegfx.c -o $@ -lm
