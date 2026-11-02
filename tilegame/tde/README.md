# TDE on the tilegame platform

TDE, the Thomson-Davis Editor 5.1v (Frank Davis, Jason Hood; **public domain**), running on the 80 x 25 text mode of tilegfx with its files from tilefs.
The editor's own sources are in `src/`, from <https://github.com/johnsonjh/tde>, built like its Unix version (`-D__UNIX__`) plus `-D__TILEGAME__`, which
takes the place of curses. `config/` holds its configuration files (`tde.cfg`, help, syntax highlighting, menus, the WordStar and CUA variants); they are looked
for in `/sd/tde/`: the program sets `TDEHOME=/sd/tde` before `tde_main` (without it the editor looks in `$HOME`, finds nothing, and runs on its built-in
defaults, whose help screen says Ctrl where the configuration uses Alt).

## What was written for the port (`os/`)

- `console.c`: the editor's console functions on a text screen of 16-bit cells (character, DOS attribute). The keyboard is `plat_getkey` (platform.h): each key
  press arrives with its USB usage code and modifier bits, which `tde_event_key` turns into the editor's key number (the scan code of the key in the same place on a PC
  keyboard), shift state and character, so that every key and every Ctrl, Shift and Alt combination of the 101-key keyboard works.
- `tde_libc.c`: the C library calls the editor makes (`fopen`, `stat`, `opendir`, `rename`, `getcwd`, ...) answered from tilefs. The editor is linked with
  `ld --wrap=NAME` (see `os/Makefile.frag`) so that its sources use the usual names and the same code runs on the desktop and on the board.
- `port.c` (from the Unix port), `criterr.c` (signals: none), `tde_os.h`.

## Changes to the editor's sources (all marked "tilegame")

- `tdestr.h`: no `<ncurses.h>`; `tde_os.h` instead; the configuration files have their plain (DOS) names, `tde.cfg` and so on.
- `main.c`: no signal handlers; input and output are never redirected.
- `memory.c`: `DWORD` is 4 bytes (`uint32_t`), as on the 32-bit systems the allocator was written for (a 64-bit `long` overran its blocks on the desktop).
- The editor assumes `char` is signed (its allocator stores -1 in a byte): build with `-fsigned-char` where the compiler's default is unsigned (ARM).

## Tests

`make test_tde`: the editor is run in a process of its own for each scenario (it keeps its state in globals) on a RAM disk with scripted key events, and the
screen, the cursor and the files afterwards are checked: typing and saving, every kind of cursor movement, prompts and dialogs, search, the directory list, menus
and help, CR LF files, big files, long lines, the keypad with and without num lock, caps lock, Ctrl/Alt combinations; the key table against an independent
table of PC scan codes; the C library layer; the console functions cell by cell. `./test_tde peek 'KEYS' FILE` shows the screen after some keys.

`make tde_ed` builds the editor with the SDL window: `./tde_ed [FOLDER]` (default `./tde_files`, which is /sd; put `tde.cfg`, `tde.hlp` and `tde.shl` from
`config/` in `FOLDER/tde/`).
