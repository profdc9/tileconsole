# BASIC on the tilegame platform

A self-contained development environment: a command prompt on the 80 x 25 text screen around **MY-BASIC** (Tony Wang, MIT; `my_basic/`, unmodified), with programs as free-form text
files on `tilefs` (the SD card on the board), edited with TDE and run from memory.

    ] LOAD hello       load hello.bas into memory (a name with no extension finds NAME.bas)
    ] LIST             show it, numbered (the numbers are for reading: the file has none)
    ] RUN              run it (Ctrl+C stops it)
    ] EDIT             start the editor (TDE) on it; back at the prompt with the file reloaded
    ] PRINT 2 + 2      anything else is BASIC and runs at once
    HELP CLS NEW LIST [from [to]] RUN LOAD SAVE [file] FILES [folder] KILL CD EDIT VER BYE

`basic_env.[ch]` is the environment (keys, screen and clock through three hooks, so a test can type at it); `basic.c` is the desktop program (`make tgbasic`, `./tgbasic [folder]`,
whose files are /sd; EDIT runs `./tde_ed` on the file); `make test_basic` is the test (typed keys, screen, files; run with AddressSanitizer and UBSan).

## MY-BASIC as it is, and how it is built

The source is MY-BASIC's own, not modified. Two of its settings are given on the command line (`BASICFLAGS` in the Makefile):

- `-Dmb_mem_tag_t=size_t`: it puts a size tag in front of every block it allocates, 16 bits by default, which leaves every block misaligned (undefined behaviour, and on the board's CPU
  some instructions fault); a pointer-sized tag keeps them aligned.
- Its default "compact mode" packs its structures, which GCC handles (members at odd addresses); `-Wno-address-of-packed-member` and the sanitizer's alignment check off say so.

## Things about MY-BASIC that are different from what you may expect

- `PRINT a, b` stays on the line; **a semicolon ends it**: `PRINT a;` prints a line. (`PRINT 2 + 2` at the prompt gets its line ended for you.)
- `:` puts several statements on one line. Strings: `left`, `mid`, `str`, `len`, ... (no `ucase`).
- **Each line typed at the prompt is a program of its own.** Its variables are gone afterwards (an undefined number is 0, an undefined string `(EMPTY)`), and so are a program's after RUN.
  MY-BASIC cannot run a line among the variables of an earlier one: a string variable can point into the text of the line it was assigned in, which is freed when the next line is loaded.
  (Found by running it under AddressSanitizer: `mb_reset` with the variables kept, or a forked interpreter, both end in a use after free.) Use a file for anything longer than a line.
- Printing a list or dictionary prints its type (`LIST`, `DICT`), not its contents.

## Limits

A program is at most `BASIC_PROGRAM_MAX` (64 KB) of text. The prompt line is as long as the screen line allows. MY-BASIC's callbacks carry no pointer, so there is one environment at a time.
