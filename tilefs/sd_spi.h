/* SPDX-License-Identifier: MIT */
/*
 * An SD / SDHC / SDXC card in SPI mode as a tilefs block device (fs_blockdev_t), for the FatFs backend. Portable: the card is reached
 * through a small transport (sd_spi_io_t: chip select, byte transfer, bus speed, a millisecond clock), so the protocol runs against
 * a software model of a card in the tests (test_sd.c) and against the SPI hardware on the board (pico/sd_spi_pico.c).
 *
 * Sector size is 512 bytes. Writes always send a valid CRC and are sent again on a CRC error. Reads check the data CRC and retry, but only
 * from a card that has agreed to CRC checking (CMD59), as only such a card has to send a valid CRC.
 *
 * Compared with ChaN's drivers in the FatFs sample projects (see README): the same 10 ms settling and 80 clocks with the card not selected
 * at 400 kHz, the same command sequence and timeouts, a chip select and a clock byte round every command, and ACMD23 before multiple block
 * writes. Not supported: MMC cards (CMD1); the socket is for SD cards. Errors: FS_ENOMEDIA when
 * nothing answers (no card), FS_EIO for everything else the card or the bus does wrong.
 */
#ifndef SD_SPI_H
#define SD_SPI_H

#include <stddef.h>
#include <stdint.h>

#include "tilefs.h"

typedef struct {
    void (*cs)(void *ctx, int asserted);                                       /* chip select: 1 = the card is selected */
    void (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n);         /* tx NULL sends 0xFF, rx NULL discards */
    void (*set_speed)(void *ctx, uint32_t hz);                                 /* the bus clock; at most this, not much less */
    uint32_t (*millis)(void *ctx);                                             /* a clock that counts milliseconds (wraps are fine) */
} sd_spi_io_t;

typedef struct {
    const sd_spi_io_t *io;
    void *io_ctx;
    uint32_t max_hz;     /* the bus speed after the card is set up: 25 MHz is the most a standard card takes */
    uint32_t sectors;    /* the card's size, known after initialisation */
    uint8_t sdhc;        /* block addressed (SDHC/SDXC); otherwise byte addressed (the driver converts) */
    uint8_t no_crc;      /* set this (after sd_spi_setup, before the card is set up) to leave CRC checking off, as simple drivers do */
    uint8_t crc_on;      /* the card agreed to check CRCs, so the CRCs of what it sends are checked too */
    uint8_t ready;
} sd_spi_t;

#define SD_SPI_INIT_HZ 400000

/* Fills in the structure; touches nothing. The card is set up when the block device's init runs (that is, when FatFs first mounts). */
void sd_spi_setup(sd_spi_t *sd, const sd_spi_io_t *io, void *io_ctx, uint32_t max_hz);

/* The block device: pass the sd_spi_t as the context. */
extern const fs_blockdev_t sd_spi_dev;

/* The pieces, for tests. */
uint8_t sd_crc7(const uint8_t *data, size_t n);        /* the command CRC, in the low 7 bits */
uint16_t sd_crc16(const uint8_t *data, size_t n);      /* CRC-16/XMODEM, the data block CRC */

#endif
