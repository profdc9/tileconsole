/* The 8 MB PSRAM on the Emu2350: QSPI, chip select 1 (GPIO47), mapped by the QMI at 0x11000000 behind the XIP cache. */
#ifndef PSRAM_H
#define PSRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSRAM_CS_PIN 47
#define PSRAM_BASE ((void *)0x11000000u)

/* Starts the PSRAM; call once, on core 0 after the system clock is set and before core 1 runs (it takes the flash interface
 * out of use for a few microseconds). Returns its size in bytes (1, 2, 4, 8 or 16 MB), 0 if there is none. */
size_t psram_init(unsigned cs_pin, unsigned max_mhz);

/* What psram_init found. */
size_t psram_size(void);

/* Writes and reads back the whole PSRAM `passes` times with an address-dependent pattern. Returns the number of words that read
 * back wrong; the speeds are in KB/s (the writes and reads are 32-bit, one after the other, through the cache). */
uint32_t psram_selftest(int passes, uint32_t *write_kbs, uint32_t *read_kbs);

#endif
