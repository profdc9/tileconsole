/* SPDX-License-Identifier: MIT */
/*
 * The transport of the SD card driver (sd_spi.h) for the RP2040 / RP2350 SPI hardware (pico-sdk). Chip select is a plain GPIO that is
 * held across the bytes of a command; the other pins belong to the SPI block. MISO gets the internal pull-up (INPUT_PULLUP): with no card
 * in the socket the line then reads 0xFF, which the driver takes for "nothing there".
 *
 * Usage:
 *     static sd_spi_pico_t port;
 *     static sd_spi_t sd;
 *     sd_spi_pico_init(&port, &sd, spi0, SD_SPI_PICO_EMUBOARD2350, 12500000);
 *     fs_fat_init(&fat, 0, &sd_spi_dev, &sd);       // the card is set up here
 */
#ifndef SD_SPI_PICO_H
#define SD_SPI_PICO_H

#include "hardware/spi.h"
#include "sd_spi.h"

typedef struct {
    spi_inst_t *spi;
    uint8_t miso, cs, sck, mosi;
} sd_spi_pins_t;

/* The EmuBoard2350: SPI0, MISO = GPIO4, CS = GPIO5, SCK = GPIO6, MOSI = GPIO7 (from the schematic). */
#define SD_SPI_PICO_EMUBOARD2350 4, 5, 6, 7

typedef struct {
    spi_inst_t *spi;
    uint8_t cs;
} sd_spi_pico_t;

/* Sets up the pins and the SPI block (at the slow start-up speed) and fills in `sd` for use with sd_spi_dev. max_hz is the speed the bus is
 * moved to once the card is set up: 25 MHz is the most a standard card takes, and a first try on a new board is better at 12.5 MHz. */
void sd_spi_pico_init(sd_spi_pico_t *port, sd_spi_t *sd, spi_inst_t *spi, uint miso, uint cs, uint sck, uint mosi, uint32_t max_hz);

extern const sd_spi_io_t sd_spi_pico_io;

#endif
