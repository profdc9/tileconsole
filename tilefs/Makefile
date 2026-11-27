CFLAGS ?= -std=c11 -Wall -Wextra -O2
FATFS = third_party/fatfs
SRC = tilefs.c fs_ram.c fs_posix.c fs_zip.c fs_blk.c fs_blk_file.c fs_fat.c fs_fat_diskio.c $(FATFS)/ff.c $(FATFS)/ffunicode.c

test_tilefs: test_tilefs.c $(SRC) tilefs.h fs_fat.h
	$(CC) $(CFLAGS) -I$(FATFS) -I. -g -fsanitize=address,undefined test_tilefs.c $(SRC) -o $@

check: test_tilefs test_sd test_zip
	./test_tilefs
	./test_zip
	./test_sd

test_zip: test_zip.c test_zip_real.h tilefs.c fs_ram.c fs_zip.c tilefs.h
	$(CC) $(CFLAGS) -I. -g -fsanitize=address,undefined test_zip.c tilefs.c fs_ram.c fs_zip.c -o $@

clean:
	rm -f test_tilefs test_sd test_zip *.o

SDSRC = sd_spi.c tilefs.c fs_ram.c fs_posix.c fs_zip.c fs_blk.c fs_blk_file.c fs_fat.c fs_fat_diskio.c $(FATFS)/ff.c $(FATFS)/ffunicode.c

test_sd: test_sd.c $(SDSRC) sd_spi.h tilefs.h fs_fat.h
	$(CC) $(CFLAGS) -I$(FATFS) -I. -g -fsanitize=address,undefined test_sd.c $(SDSRC) -o $@

check-sd: test_sd
	./test_sd
