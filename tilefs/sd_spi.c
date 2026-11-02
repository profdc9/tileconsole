/* SPDX-License-Identifier: MIT */
#include "sd_spi.h"

#include <string.h>

#define CMD0 0       /* GO_IDLE_STATE */
#define CMD8 8       /* SEND_IF_COND */
#define CMD9 9       /* SEND_CSD */
#define CMD12 12     /* STOP_TRANSMISSION */
#define CMD16 16     /* SET_BLOCKLEN */
#define CMD17 17     /* READ_SINGLE_BLOCK */
#define CMD18 18     /* READ_MULTIPLE_BLOCK */
#define CMD24 24     /* WRITE_BLOCK */
#define CMD25 25     /* WRITE_MULTIPLE_BLOCK */
#define CMD55 55     /* APP_CMD: the next command is an ACMD */
#define CMD58 58     /* READ_OCR */
#define CMD59 59     /* CRC_ON_OFF */
#define ACMD23 23    /* SET_WR_BLK_ERASE_COUNT */
#define ACMD41 41    /* SD_SEND_OP_COND */

#define R1_IDLE 0x01
#define R1_ILLEGAL 0x04
#define TOKEN_SINGLE 0xFE    /* start of a data block (and of a single block write) */
#define TOKEN_MULTI 0xFC     /* start of a block of a multiple block write */
#define TOKEN_STOP 0xFD      /* end of a multiple block write */

#define SETTLE_MS 10
#define INIT_MS 1000         /* the card has this long to leave the idle state */
#define READ_MS 200
#define BUSY_MS 500
#define RETRIES 3

uint8_t sd_crc7(const uint8_t *data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            const int feedback = ((crc >> 6) & 1) ^ ((data[i] >> b) & 1);
            crc = (uint8_t)((crc << 1) & 0x7F);
            if (feedback)
                crc ^= 0x09;
        }
    }
    return crc;
}

uint16_t sd_crc16(const uint8_t *data, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc = (uint16_t)((crc >> 8) | (crc << 8));
        crc ^= data[i];
        crc ^= (crc & 0xFF) >> 4;
        crc ^= (uint16_t)(crc << 12);
        crc ^= (uint16_t)((crc & 0xFF) << 5);
    }
    return crc;
}

void sd_spi_setup(sd_spi_t *sd, const sd_spi_io_t *io, void *io_ctx, uint32_t max_hz)
{
    memset(sd, 0, sizeof *sd);
    sd->io = io;
    sd->io_ctx = io_ctx;
    sd->max_hz = max_hz;
}

/* ---- the bus ---- */

static uint8_t xfer1(const sd_spi_t *sd, uint8_t tx)
{
    uint8_t rx = 0xFF;
    sd->io->xfer(sd->io_ctx, &tx, &rx, 1);
    return rx;
}

/* The card holds the line low while it is busy. Selected. */
static int wait_ready(const sd_spi_t *sd, uint32_t ms)
{
    const uint32_t t0 = sd->io->millis(sd->io_ctx);
    do {
        if (xfer1(sd, 0xFF) == 0xFF)
            return FS_OK;
    } while (sd->io->millis(sd->io_ctx) - t0 < ms);
    return FS_EIO;
}

/* Selects the card and clocks one byte, as ChaN's drivers do: the card only drives the line once it has been selected for a clock, and until then a
 * pulled-up line reads 0xFF, which must not be taken for "ready". */
static void select_card(const sd_spi_t *sd)
{
    sd->io->cs(sd->io_ctx, 1);
    xfer1(sd, 0xFF);
}

/* The card needs eight more clocks after chip select goes away to let go of the line. */
static void release_card(const sd_spi_t *sd)
{
    sd->io->cs(sd->io_ctx, 0);
    xfer1(sd, 0xFF);
}

/* Sends a command and returns its R1 response (0xFF if there was none). The card is selected. */
static uint8_t send_cmd(const sd_spi_t *sd, uint8_t cmd, uint32_t arg)
{
    uint8_t frame[7] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0, 0xFF};
    if (cmd != CMD12 && wait_ready(sd, BUSY_MS) != FS_OK)
        return 0xFF;
    frame[5] = (uint8_t)((sd_crc7(frame, 5) << 1) | 1);
    sd->io->xfer(sd->io_ctx, frame, NULL, cmd == CMD12 ? 7 : 6);   /* CMD12: one more byte, which the card sends garbage for */
    uint8_t r = 0xFF;
    for (int i = 0; i < 10; i++) {
        r = xfer1(sd, 0xFF);
        if (!(r & 0x80))
            break;
    }
    return r;
}

static uint8_t send_acmd(const sd_spi_t *sd, uint8_t cmd, uint32_t arg)
{
    const uint8_t r = send_cmd(sd, CMD55, 0);
    if (r > R1_IDLE)
        return r;
    release_card(sd);   /* each command in its own chip select, as in ChaN's drivers */
    select_card(sd);
    return send_cmd(sd, cmd, arg);
}

/* ---- data blocks ---- */

/* Waits for the start token of a data block. */
static int wait_token(const sd_spi_t *sd)
{
    const uint32_t t0 = sd->io->millis(sd->io_ctx);
    do {
        const uint8_t t = xfer1(sd, 0xFF);
        if (t == TOKEN_SINGLE)
            return FS_OK;
        if (t != 0xFF)   /* a data error token (0x01..0x0F) or anything else that is not a token */
            return FS_EIO;
    } while (sd->io->millis(sd->io_ctx) - t0 < READ_MS);
    return FS_EIO;
}

static int read_data(const sd_spi_t *sd, uint8_t *buf, size_t n)
{
    uint8_t crc[2];
    int e = wait_token(sd);
    if (e)
        return e;
    sd->io->xfer(sd->io_ctx, NULL, buf, n);
    sd->io->xfer(sd->io_ctx, NULL, crc, 2);
    if (!sd->crc_on)
        return FS_OK;   /* a card with CRC checking off may send anything here */
    return sd_crc16(buf, n) == (uint16_t)((crc[0] << 8) | crc[1]) ? FS_OK : FS_EIO;
}

static int write_data(const sd_spi_t *sd, const uint8_t *buf, uint8_t token, int *crc_error)
{
    const uint16_t crc = sd_crc16(buf, 512);
    const uint8_t tail[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
    xfer1(sd, token);
    sd->io->xfer(sd->io_ctx, buf, NULL, 512);
    sd->io->xfer(sd->io_ctx, tail, NULL, 2);
    const uint8_t resp = xfer1(sd, 0xFF);
    *crc_error = (resp & 0x1F) == 0x0B;
    if ((resp & 0x1F) != 0x05)
        return FS_EIO;
    return wait_ready(sd, BUSY_MS);
}

/* ---- setting up the card ---- */

static int read_csd(sd_spi_t *sd)
{
    uint8_t csd[16];
    select_card(sd);
    if (send_cmd(sd, CMD9, 0) != 0 || read_data(sd, csd, 16) != FS_OK) {
        release_card(sd);
        return FS_EIO;
    }
    release_card(sd);
    if ((csd[0] >> 6) == 1) {   /* version 2: SDHC and SDXC; the size is (C_SIZE + 1) * 512 KiB */
        const uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        sd->sectors = (c_size + 1) * 1024;
    } else if ((csd[0] >> 6) == 0) {   /* version 1: (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes */
        const unsigned read_bl_len = csd[5] & 0x0F;
        const uint32_t c_size = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        const unsigned c_size_mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
        const uint64_t blocks = (uint64_t)(c_size + 1) << (c_size_mult + 2);
        const uint64_t bytes = blocks << read_bl_len;
        sd->sectors = (uint32_t)(bytes / 512);
    } else {
        return FS_EIO;
    }
    return sd->sectors ? FS_OK : FS_EIO;
}

static int card_init(void *ctx)
{
    sd_spi_t *sd = ctx;
    uint8_t r;
    sd->ready = 0;
    sd->io->set_speed(sd->io_ctx, SD_SPI_INIT_HZ);
    sd->io->cs(sd->io_ctx, 0);
    {   /* the card has 10 ms to settle after power-up, then needs at least 74 clocks with it not selected: clock for the 10 ms (the bound is
         * for a transport whose clock does not move), and then 80 more */
        const uint32_t t0 = sd->io->millis(sd->io_ctx);
        for (int i = 0; i < 2000 && sd->io->millis(sd->io_ctx) - t0 < SETTLE_MS; i++)
            xfer1(sd, 0xFF);
        sd->io->xfer(sd->io_ctx, NULL, NULL, 10);
    }
    /* into SPI mode, the idle state */
    int idle = 0;
    for (int i = 0; i < 5 && !idle; i++) {
        select_card(sd);
        idle = send_cmd(sd, CMD0, 0) == R1_IDLE;
        release_card(sd);
    }
    if (!idle)
        return FS_ENOMEDIA;
    /* the interface condition tells a version 2 card (voltage 2.7-3.6 V, check pattern echoed) from a version 1 one, which rejects it */
    int v2 = 0;
    uint8_t tail[4] = {0};
    select_card(sd);
    r = send_cmd(sd, CMD8, 0x1AA);
    if (r == R1_IDLE) {
        sd->io->xfer(sd->io_ctx, NULL, tail, 4);
        v2 = 1;
        if ((tail[2] & 0x0F) != 0x01 || tail[3] != 0xAA) {
            release_card(sd);
            return FS_EIO;
        }
    } else if (!(r & R1_ILLEGAL)) {
        release_card(sd);
        return FS_EIO;
    }
    release_card(sd);
    /* leave the idle state; a version 2 card is asked for high capacity */
    const uint32_t t0 = sd->io->millis(sd->io_ctx);
    do {
        select_card(sd);
        r = send_acmd(sd, ACMD41, v2 ? 0x40000000u : 0);
        release_card(sd);
    } while (r != 0 && sd->io->millis(sd->io_ctx) - t0 < INIT_MS);
    if (r != 0)
        return FS_EIO;
    sd->sdhc = 0;
    if (v2) {   /* the card capacity status bit of the OCR says it is block addressed */
        uint8_t ocr[4] = {0};
        select_card(sd);
        r = send_cmd(sd, CMD58, 0);
        if (r == 0)
            sd->io->xfer(sd->io_ctx, NULL, ocr, 4);
        release_card(sd);
        if (r != 0)
            return FS_EIO;
        sd->sdhc = (ocr[0] & 0x40) != 0;
    }
    if (!sd->sdhc) {
        select_card(sd);
        r = send_cmd(sd, CMD16, 512);
        release_card(sd);
        if (r != 0)
            return FS_EIO;
    }
    sd->crc_on = 0;
    if (!sd->no_crc) {   /* ask the card to check CRCs. Only a card that agrees is obliged to send a valid CRC with what it reads, so only then is it checked */
        select_card(sd);
        sd->crc_on = send_cmd(sd, CMD59, 1) == 0;
        release_card(sd);
    }
    const int e = read_csd(sd);
    if (e)
        return e;
    sd->io->set_speed(sd->io_ctx, sd->max_hz);
    sd->ready = 1;
    return FS_OK;
}

/* ---- reading and writing ---- */

static uint32_t address(const sd_spi_t *sd, uint32_t sector)
{
    return sd->sdhc ? sector : sector * 512u;
}

static int card_read(void *ctx, void *buf, uint32_t sector, uint32_t count)
{
    sd_spi_t *sd = ctx;
    uint8_t *p = buf;
    uint32_t done = 0;
    int tries = 0;
    if (!sd->ready)
        return FS_ENOMEDIA;
    if (sector > sd->sectors || count > sd->sectors - sector)
        return FS_EINVAL;
    while (done < count) {
        const uint32_t left = count - done;
        int e = FS_OK;
        select_card(sd);
        if (send_cmd(sd, left == 1 ? CMD17 : CMD18, address(sd, sector + done)) != 0) {
            e = FS_EIO;
        } else {
            while (done < count) {
                e = read_data(sd, p + (size_t)done * 512, 512);
                if (e)
                    break;
                done++;
            }
            if (left > 1)
                send_cmd(sd, CMD12, 0);   /* the card goes on sending blocks until told to stop; its answer follows */
        }
        release_card(sd);
        if (e && ++tries >= RETRIES)
            return e;
    }
    return FS_OK;
}

static int card_write(void *ctx, const void *buf, uint32_t sector, uint32_t count)
{
    sd_spi_t *sd = ctx;
    const uint8_t *p = buf;
    uint32_t done = 0;
    int tries = 0;
    if (!sd->ready)
        return FS_ENOMEDIA;
    if (sector > sd->sectors || count > sd->sectors - sector)
        return FS_EINVAL;
    while (done < count) {
        const uint32_t left = count - done;
        int e = FS_OK, crc_error = 0;
        select_card(sd);
        if (left > 1)
            send_acmd(sd, ACMD23, left);   /* tell the card how many blocks are coming, so that it can erase them in advance; not all cards care */
        if (send_cmd(sd, left == 1 ? CMD24 : CMD25, address(sd, sector + done)) != 0) {
            e = FS_EIO;
        } else {
            xfer1(sd, 0xFF);   /* a gap of at least one byte before the data */
            while (done < count) {
                e = write_data(sd, p + (size_t)done * 512, left == 1 ? TOKEN_SINGLE : TOKEN_MULTI, &crc_error);
                if (e)
                    break;
                done++;
            }
            if (left > 1) {   /* end the multiple block write, also after an error, which leaves the card waiting for more */
                xfer1(sd, 0xFF);
                xfer1(sd, TOKEN_STOP);
                xfer1(sd, 0xFF);
                if (wait_ready(sd, BUSY_MS) != FS_OK && !e)
                    e = FS_EIO;
            }
        }
        release_card(sd);
        if (e && (!crc_error || ++tries >= RETRIES))   /* a block that arrived damaged is sent again; a failed write is not */
            return e;
    }
    return FS_OK;
}

static int card_sync(void *ctx)
{
    sd_spi_t *sd = ctx;
    if (!sd->ready)
        return FS_ENOMEDIA;
    select_card(sd);
    const int e = wait_ready(sd, BUSY_MS);
    release_card(sd);
    return e;
}

static uint32_t card_sectors(void *ctx)
{
    return ((sd_spi_t *)ctx)->sectors;
}

const fs_blockdev_t sd_spi_dev = {card_init, card_read, card_write, card_sync, card_sectors};
