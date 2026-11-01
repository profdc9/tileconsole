/* SPDX-License-Identifier: MIT */
/* Tests of the SD card driver (sd_spi.c) against a software model of a card in SPI mode, and of the whole stack (FatFs on the SD driver
 * on the model) checked by fsck. The model checks what a real card would insist on: at least 74 clocks before CMD0, a slow bus until
 * the card is initialised, command CRCs, data CRCs, byte or block addresses, the busy line, and answers only when selected. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs_fat.h"
#include "sd_spi.h"
#include "tilefs.h"

static unsigned long checks;

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define EQ(a, b) do { const long long va_ = (long long)(a), vb_ = (long long)(b); checks++; \
    if (va_ != vb_) { fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); exit(1); } } while (0)

#define MAX_HZ 25000000u

/* ---- the card ---- */

typedef struct {
    uint8_t *img;
    uint32_t sectors;
    int sdhc;                  /* block addressed; the card answers a version 2 CMD8 */
    int v1;                    /* a version 1 card: CMD8 is an illegal command, always byte addressed */
    unsigned bl_len, mult;     /* what a version 1 CSD says: 2^bl_len bytes per block, 2^(mult+2) blocks per C_SIZE step */
    /* state */
    int cs, spi_mode, idle, app, crc_on, ccs;
    uint8_t cmd[6];
    int cmd_n;
    enum { RX_CMD, RX_TOKEN, RX_DATA } mode;
    int wr_multi;
    uint32_t wr_sector;
    uint8_t blk[514];
    int blk_n;
    uint8_t out[1200];
    int out_n, out_pos;
    long busy_left;
    int rd_multi;
    uint32_t rd_sector;
    /* faults */
    int no_card, never_ready, acmd41_polls, read_token_errors, read_crc_errors, write_crc_errors, write_failures, stuck_busy;
    int ignore_stop;
    int reject_cmd59;          /* a card that does not know CMD59 */
    int garbage_crc_when_off;  /* with CRC checking off the data CRC of a read is whatever (the specification does not promise a valid one) */
    int fault_sector;          /* read faults apply to this sector only (the card reads ahead, so counting blocks would mislead); -1: any */
    /* what was seen */
    int log[400], log_n;
    unsigned cs_high_clocks_before_cmd0;
    uint32_t hz;
    double us;
    int bad_speed_use;         /* commands sent at a speed the card cannot follow */
    int bad_crc_seen;
    int acmd23_calls;
    uint32_t acmd23_arg;
    double first_clock_us;     /* when the first clock was seen, to check the 10 ms settling before CMD0 */
    int clocked;
    int bad_protocol;          /* a multiple block write abandoned without its stop token; selected again with no clocks between */
    unsigned high_bytes;       /* bytes clocked since the card was last deselected */
    uint32_t reads, writes;
} card_t;

static void queue(card_t *c, const void *p, int n)
{
    memcpy(c->out + c->out_n, p, (size_t)n);
    c->out_n += n;
}

static void queue_block(card_t *c, uint32_t sector)
{
    uint8_t b[1 + 512 + 2];
    b[0] = 0xFE;
    memcpy(b + 1, c->img + (size_t)sector * 512, 512);
    uint16_t crc = sd_crc16(b + 1, 512);
    const int hit = c->fault_sector < 0 || (uint32_t)c->fault_sector == sector;
    if (!c->crc_on && c->garbage_crc_when_off)
        crc = 0xBEEF;
    if (hit && c->read_crc_errors > 0) {
        c->read_crc_errors--;
        crc ^= 0x0100;
    }
    b[513] = (uint8_t)(crc >> 8);
    b[514] = (uint8_t)crc;
    if (hit && c->read_token_errors > 0) {
        c->read_token_errors--;
        const uint8_t err = 0x01;   /* a data error token instead of the block */
        queue(c, &err, 1);
        return;
    }
    queue(c, b, 515);
    c->reads++;
}

static void queue_csd(card_t *c)
{
    uint8_t csd[18] = {0};
    if (c->v1) {
        const uint32_t cs = (uint32_t)(((uint64_t)c->sectors * 512 >> c->bl_len >> (c->mult + 2)) - 1);   /* C_SIZE */
        csd[0] = 0x00;
        csd[5] = (uint8_t)c->bl_len;
        csd[6] = (uint8_t)((cs >> 10) & 3);
        csd[7] = (uint8_t)(cs >> 2);
        csd[8] = (uint8_t)((cs & 3) << 6);
        csd[9] = (uint8_t)((c->mult >> 1) & 3);
        csd[10] = (uint8_t)((c->mult & 1) << 7);
    } else {
        const uint32_t cs = c->sectors / 1024 - 1;
        csd[0] = 0x40;
        csd[7] = (uint8_t)((cs >> 16) & 0x3F);
        csd[8] = (uint8_t)(cs >> 8);
        csd[9] = (uint8_t)cs;
    }
    const uint16_t crc = sd_crc16(csd, 16);
    csd[16] = (uint8_t)(crc >> 8);
    csd[17] = (uint8_t)crc;
    const uint8_t tok = 0xFE;
    queue(c, &tok, 1);
    queue(c, csd, 18);
}

static void r1(card_t *c, uint8_t r)
{
    const uint8_t gap[2] = {0xFF, r};   /* the card answers one byte late, as they do */
    queue(c, gap, 2);
}

static int sector_of(card_t *c, uint32_t arg, uint32_t *sector, uint8_t *err)
{
    if (!c->sdhc) {
        if (arg % 512) {
            *err = 0x20;   /* address error */
            return 0;
        }
        arg /= 512;
    }
    if (arg >= c->sectors) {
        *err = 0x40;       /* parameter error */
        return 0;
    }
    *sector = arg;
    return 1;
}

static void exec(card_t *c)
{
    const int idx = c->cmd[0] & 0x3F;
    const uint32_t arg = (uint32_t)c->cmd[1] << 24 | (uint32_t)c->cmd[2] << 16 | (uint32_t)c->cmd[3] << 8 | c->cmd[4];
    const int was_app = c->app;
    uint8_t err;
    uint32_t sector;
    c->app = 0;
    if (c->log_n < 400)
        c->log[c->log_n++] = was_app ? 100 + idx : idx;
    if (c->hz > SD_SPI_INIT_HZ && c->idle)
        c->bad_speed_use++;   /* before the card is set up the bus must be slow */
    if (idx == 0 && !c->spi_mode && c->cs_high_clocks_before_cmd0 < 10)
        c->bad_speed_use += 100;   /* not enough clocks to wake it */
    if (idx == 0 && !c->spi_mode && c->us - c->first_clock_us < 10000.0)
        c->bad_speed_use += 1000;  /* the card was not given its 10 ms to settle */
    if (!c->spi_mode && idx != 0)
        return;   /* an SD mode card does not hear these */
    if ((idx == 0 || idx == 8 || c->crc_on) && (c->cmd[5] != (uint8_t)((sd_crc7(c->cmd, 5) << 1) | 1))) {
        c->bad_crc_seen++;
        r1(c, 0x09 | (c->idle ? 1 : 0));   /* command CRC error */
        return;
    }
    if (!(c->cmd[5] & 1))
        c->bad_crc_seen++;
    if (was_app) {
        if (idx == 41) {
            if (c->never_ready || c->acmd41_polls-- > 0) {
                r1(c, 0x01);
            } else {
                c->idle = 0;
                c->ccs = c->sdhc && (arg & 0x40000000);
                r1(c, 0x00);
            }
            return;
        }
        if (idx == 23) {
            c->acmd23_calls++;
            c->acmd23_arg = arg;
            r1(c, 0x00);
            return;
        }
        r1(c, 0x04);
        return;
    }
    switch (idx) {
    case 0:
        c->spi_mode = 1;
        c->idle = 1;
        c->crc_on = 0;
        c->mode = RX_CMD;
        c->rd_multi = 0;
        r1(c, 0x01);
        break;
    case 8:
        if (c->v1) {
            r1(c, 0x05);
        } else {
            r1(c, 0x01);
            const uint8_t r7[4] = {0, 0, (uint8_t)((arg >> 8) & 0x0F), (uint8_t)arg};
            queue(c, r7, 4);
        }
        break;
    case 9:
        r1(c, 0x00);
        queue_csd(c);
        break;
    case 12:
        if (c->ignore_stop) {
            r1(c, 0x00);
            break;
        }
        c->rd_multi = 0;
        c->out_n = c->out_pos = 0;
        r1(c, 0x00);   /* the byte before is the stuff byte the host discards */
        c->busy_left = 4;
        break;
    case 16:
        r1(c, arg == 512 ? (c->idle ? 1 : 0) : 0x40);
        break;
    case 17:
        if (!sector_of(c, arg, &sector, &err)) {
            r1(c, err);
            break;
        }
        r1(c, 0x00);
        queue_block(c, sector);
        break;
    case 18:
        if (!sector_of(c, arg, &sector, &err)) {
            r1(c, err);
            break;
        }
        r1(c, 0x00);
        c->rd_multi = 1;
        c->rd_sector = sector;
        queue_block(c, c->rd_sector++);
        break;
    case 24:
    case 25:
        if (!sector_of(c, arg, &sector, &err)) {
            r1(c, err);
            break;
        }
        r1(c, 0x00);
        c->mode = RX_TOKEN;
        c->wr_multi = idx == 25;
        c->wr_sector = sector;
        break;
    case 55:
        c->app = 1;
        r1(c, c->idle ? 0x01 : 0x00);
        break;
    case 58: {
        r1(c, c->idle ? 0x01 : 0x00);
        const uint8_t ocr[4] = {(uint8_t)((c->idle ? 0 : 0x80) | (c->ccs ? 0x40 : 0)), 0xFF, 0x80, 0};
        queue(c, ocr, 4);
        break;
    }
    case 59:
        if (c->reject_cmd59) {
            r1(c, 0x04);
            break;
        }
        c->crc_on = arg & 1;
        r1(c, c->idle ? 0x01 : 0x00);
        break;
    default:
        r1(c, 0x04);
    }
}

static uint8_t card_byte(card_t *c, uint8_t tx)
{
    if (c->no_card)
        return 0xFF;
    if (!c->cs) {
        if (!c->spi_mode)
            c->cs_high_clocks_before_cmd0++;
        c->high_bytes++;
        return 0xFF;
    }
    uint8_t rx = 0xFF;
    if (c->out_pos < c->out_n) {
        rx = c->out[c->out_pos++];
        if (c->out_pos == c->out_n)
            c->out_pos = c->out_n = 0;
    } else if (c->busy_left > 0) {
        c->busy_left--;
        rx = 0x00;
    } else if (c->rd_multi && c->mode == RX_CMD) {   /* the card goes on with the next block until told to stop */
        queue_block(c, c->rd_sector++ % c->sectors);
    }
    switch (c->mode) {
    case RX_CMD:
        if (c->cmd_n == 0 && (tx & 0xC0) != 0x40)
            break;
        c->cmd[c->cmd_n++] = tx;
        if (c->cmd_n == 6) {
            c->cmd_n = 0;
            exec(c);
        }
        break;
    case RX_TOKEN:
        if (tx == (c->wr_multi ? 0xFC : 0xFE)) {
            c->mode = RX_DATA;
            c->blk_n = 0;
        } else if (tx == 0xFD && c->wr_multi) {
            c->mode = RX_CMD;
            c->busy_left = c->stuck_busy ? 1000000000L : 4;
        }
        break;
    case RX_DATA:
        c->blk[c->blk_n++] = tx;
        if (c->blk_n == 514) {
            uint8_t resp = 0x05;
            if (c->write_crc_errors > 0) {
                c->write_crc_errors--;
                resp = 0x0B;
            } else if (c->crc_on && sd_crc16(c->blk, 512) != (uint16_t)((c->blk[512] << 8) | c->blk[513])) {
                c->bad_crc_seen++;
                resp = 0x0B;
            } else if (c->write_failures > 0) {
                c->write_failures--;
                resp = 0x0D;
            } else {
                memcpy(c->img + (size_t)c->wr_sector * 512, c->blk, 512);
                c->writes++;
                c->wr_sector++;
            }
            c->out[0] = resp;
            c->out_n = 1;
            c->out_pos = 0;
            c->busy_left = c->stuck_busy ? 1000000000L : 6;
            c->mode = c->wr_multi ? RX_TOKEN : RX_CMD;
        }
        break;
    }
    return rx;
}

/* ---- the transport the driver sees ---- */

static void io_cs(void *ctx, int a)
{
    card_t *c = ctx;
    if (c->cs != a) {
        c->cmd_n = 0;
        if (a && c->spi_mode && !c->no_card && c->high_bytes == 0)
            c->bad_protocol++;   /* the card needs eight clocks after it was deselected to let go of the line */
        if (!a) {
            c->high_bytes = 0;
            if (c->mode == RX_TOKEN && c->wr_multi && !c->stuck_busy)
                c->bad_protocol++;   /* a multiple block write is ended by the stop token, not by deselecting */
            c->mode = RX_CMD;   /* leaving a write half way abandons it */
        }
    }
    c->cs = a;
}

static void io_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n)
{
    card_t *c = ctx;
    if (!c->clocked) {
        c->clocked = 1;
        c->first_clock_us = c->us;
    }
    for (size_t i = 0; i < n; i++) {
        const uint8_t r = card_byte(c, tx ? tx[i] : 0xFF);
        if (rx)
            rx[i] = r;
    }
    c->us += (double)n * 8.0 * 1e6 / (double)(c->hz ? c->hz : 1);
}

static void io_speed(void *ctx, uint32_t hz)
{
    ((card_t *)ctx)->hz = hz;
}

static uint32_t io_millis(void *ctx)
{
    return (uint32_t)(((card_t *)ctx)->us / 1000.0);
}

static const sd_spi_io_t io = {io_cs, io_xfer, io_speed, io_millis};

static uint8_t *image(uint32_t sectors)
{
    uint8_t *p = calloc(sectors, 512);
    CHECK(p != NULL);
    return p;
}

static void make_card(card_t *c, uint32_t sectors, int sdhc, int v1)
{
    memset(c, 0, sizeof *c);
    c->fault_sector = -1;
    c->hz = MAX_HZ;   /* as a card left running at speed by an earlier session would be: the driver must slow the bus down itself */
    c->img = image(sectors);
    c->sectors = sectors;
    c->sdhc = sdhc;
    c->v1 = v1;
    c->bl_len = 9;
    c->mult = 7;
    c->acmd41_polls = 2;
}

/* ---- tests ---- */

static void test_crc(void)
{
    const uint8_t cmd0[5] = {0x40, 0, 0, 0, 0}, cmd8[5] = {0x48, 0, 0, 0x01, 0xAA}, cmd17[5] = {0x51, 0, 0, 0, 0};
    EQ((sd_crc7(cmd0, 5) << 1) | 1, 0x95);   /* the well known constants of CMD0 and CMD8 */
    EQ((sd_crc7(cmd8, 5) << 1) | 1, 0x87);
    EQ((sd_crc7(cmd17, 5) << 1) | 1, 0x01 | (0x2A << 1));   /* the table in the SD simplified specification: CMD17 arg 0 is 0x55 */
    EQ(sd_crc16((const uint8_t *)"123456789", 9), 0x31C3);   /* CRC-16/XMODEM check value */
    EQ(sd_crc16(NULL, 0), 0);
    uint8_t ff[512];
    memset(ff, 0xFF, sizeof ff);
    EQ(sd_crc16(ff, 512), 0x7FA1);   /* known: a block of 0xFF */
    uint8_t z[512] = {0};
    EQ(sd_crc16(z, 512), 0);
}

static void test_init(uint32_t sectors, int sdhc, int v1, unsigned bl_len, unsigned mult)
{
    card_t c;
    sd_spi_t sd;
    make_card(&c, sectors, sdhc, v1);
    c.bl_len = bl_len;
    c.mult = mult;
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(sd_spi_dev.init(&sd), 0);
    EQ(sd.sectors, sectors);
    EQ(sd.sdhc, sdhc && !v1);
    EQ(sd_spi_dev.sectors(&sd), sectors);
    EQ(c.hz, MAX_HZ);                        /* sped up once the card was set up */
    EQ(c.bad_speed_use, 0);                  /* and not before */
    EQ(c.bad_crc_seen, 0);
    EQ(c.bad_protocol, 0);
    CHECK(c.cs_high_clocks_before_cmd0 >= 10);   /* 80 clocks with the card not selected */
    EQ(c.log[0], 0);                         /* CMD0 first */
    EQ(c.idle, 0);
    EQ(c.crc_on, 1);                         /* CMD59 turned the card's CRC checking on */
    /* CMD8 then the application commands, the OCR only from a version 2 card, CMD16 only for a byte addressed one */
    int seen8 = 0, seen58 = 0, seen16 = 0, seen41 = 0, seen9 = 0;
    for (int i = 0; i < c.log_n; i++) {
        seen8 += c.log[i] == 8;
        seen58 += c.log[i] == 58;
        seen16 += c.log[i] == 16;
        seen41 += c.log[i] == 141;
        seen9 += c.log[i] == 9;
    }
    EQ(seen8, 1);
    EQ(seen58, v1 ? 0 : 1);
    EQ(seen16, (sdhc && !v1) ? 0 : 1);
    EQ(seen41, 3);                           /* two answers of "still idle" and then ready */
    EQ(seen9, 1);
    free(c.img);
}

static void test_blocks(int sdhc, int v1)
{
    card_t c;
    sd_spi_t sd;
    const uint32_t sectors = 4096;
    uint8_t *shadow = image(sectors);
    uint8_t buf[512 * 20], back[512 * 20];
    unsigned long seed = 12345;
    make_card(&c, sectors, sdhc, v1);
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(sd_spi_dev.init(&sd), 0);
    for (int round = 0; round < 300; round++) {
        seed = seed * 6364136223846793005ul + 1442695040888963407ul;
        uint32_t n = 1 + (uint32_t)((seed >> 33) % 20);
        if ((seed >> 20) % 3 == 0)
            n = 1;
        const uint32_t s = (uint32_t)((seed >> 40) % (sectors - n + 1));
        for (size_t i = 0; i < n * 512; i++) {
            seed = seed * 6364136223846793005ul + 1442695040888963407ul;
            buf[i] = (uint8_t)(seed >> 56);
        }
        EQ(sd_spi_dev.write(&sd, buf, s, n), 0);
        memcpy(shadow + (size_t)s * 512, buf, (size_t)n * 512);
        const uint32_t rs = (uint32_t)((seed >> 12) % (sectors - n + 1)), rn = n;
        EQ(sd_spi_dev.read(&sd, back, rs, rn), 0);
        CHECK(!memcmp(back, shadow + (size_t)rs * 512, (size_t)rn * 512));
    }
    CHECK(!memcmp(c.img, shadow, (size_t)sectors * 512));   /* the card holds exactly what was written, nothing else touched */
    /* the edges */
    EQ(sd_spi_dev.write(&sd, buf, sectors - 1, 1), 0);
    EQ(sd_spi_dev.read(&sd, back, sectors - 1, 1), 0);
    EQ(sd_spi_dev.write(&sd, buf, sectors - 2, 2), 0);
    EQ(sd_spi_dev.read(&sd, back, 0, 2), 0);
    EQ(sd_spi_dev.read(&sd, back, sectors, 1), FS_EINVAL);
    EQ(sd_spi_dev.read(&sd, back, sectors - 1, 2), FS_EINVAL);
    EQ(sd_spi_dev.write(&sd, buf, sectors - 1, 2), FS_EINVAL);
    EQ(sd_spi_dev.write(&sd, buf, 0xFFFFFFF0u, 100), FS_EINVAL);
    EQ(sd_spi_dev.sync(&sd), 0);
    EQ(c.bad_crc_seen, 0);
    EQ(c.bad_protocol, 0);
    CHECK(c.acmd23_calls > 0);   /* multiple block writes were announced with ACMD23 */
    free(c.img);
    free(shadow);
}

/* Cards that are less well behaved than the model above: ones that do not know CMD59, ones that send junk in the CRC of a block when CRC
 * checking is off. The driver must work with them, as ChaN's does. */
static void test_odd_cards(void)
{
    uint8_t buf[512 * 6], back[512 * 6];
    for (unsigned i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 7);
    for (int variant = 0; variant < 4; variant++) {
        card_t c;
        sd_spi_t sd;
        make_card(&c, 4096, variant & 1, 0);
        c.reject_cmd59 = variant >= 2;             /* 2, 3: the card does not know CMD59, so CRC checking stays off */
        c.garbage_crc_when_off = 1;                /* ... and then it sends a CRC that is not one */
        sd_spi_setup(&sd, &io, &c, MAX_HZ);
        EQ(sd_spi_dev.init(&sd), 0);
        EQ(sd.crc_on, variant < 2);                /* checking only when the card agreed to it */
        EQ(sd_spi_dev.write(&sd, buf, 100, 6), 0);
        EQ(c.acmd23_arg, 6);                       /* announced */
        EQ(sd_spi_dev.read(&sd, back, 100, 6), 0);
        CHECK(!memcmp(buf, back, sizeof buf));
        EQ(sd_spi_dev.read(&sd, back, 101, 1), 0);
        CHECK(!memcmp(buf + 512, back, 512));
        EQ(c.bad_protocol, 0);
        free(c.img);
    }
    /* checking left off on request, from a card that would have agreed */
    card_t c;
    sd_spi_t sd;
    make_card(&c, 4096, 1, 0);
    c.garbage_crc_when_off = 1;
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    sd.no_crc = 1;
    EQ(sd_spi_dev.init(&sd), 0);
    EQ(sd.crc_on, 0);
    EQ(c.crc_on, 0);
    EQ(sd_spi_dev.write(&sd, buf, 5, 2), 0);
    EQ(sd_spi_dev.read(&sd, back, 5, 2), 0);
    CHECK(!memcmp(buf, back, 1024));
    for (int i = 0; i < c.log_n; i++)
        CHECK(c.log[i] != 59);
    free(c.img);
}

static void test_faults(void)
{
    card_t c;
    sd_spi_t sd;
    uint8_t buf[512 * 4], back[512 * 4];
    memset(buf, 0x3C, sizeof buf);
    /* no card: nothing answers */
    make_card(&c, 2048, 1, 0);
    c.no_card = 1;
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(sd_spi_dev.init(&sd), FS_ENOMEDIA);
    EQ(sd_spi_dev.read(&sd, back, 0, 1), FS_ENOMEDIA);   /* an uninitialised driver does not touch the bus */
    EQ(sd_spi_dev.write(&sd, buf, 0, 1), FS_ENOMEDIA);
    EQ(sd_spi_dev.sync(&sd), FS_ENOMEDIA);
    free(c.img);
    /* a card that never leaves the idle state */
    make_card(&c, 2048, 1, 0);
    c.never_ready = 1;
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(sd_spi_dev.init(&sd), FS_EIO);
    CHECK(c.us / 1000 >= 1000 && c.us / 1000 < 1500);   /* it waited its second and no more */
    free(c.img);
    /* transient read faults are retried, lasting ones are reported */
    make_card(&c, 2048, 1, 0);
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(sd_spi_dev.init(&sd), 0);
    EQ(sd_spi_dev.write(&sd, buf, 10, 4), 0);
    c.fault_sector = 10;
    c.read_token_errors = 1;
    EQ(sd_spi_dev.read(&sd, back, 10, 1), 0);
    CHECK(!memcmp(back, buf, 512));
    c.read_crc_errors = 1;
    EQ(sd_spi_dev.read(&sd, back, 10, 1), 0);
    c.fault_sector = 12;
    c.read_crc_errors = 2;
    EQ(sd_spi_dev.read(&sd, back, 10, 4), 0);   /* in a multiple block read: carries on from the block that failed */
    CHECK(!memcmp(back, buf, 512 * 4));
    c.fault_sector = 11;
    c.read_token_errors = 1;
    EQ(sd_spi_dev.read(&sd, back, 10, 4), 0);
    CHECK(!memcmp(back, buf, 512 * 4));
    c.fault_sector = 10;
    c.read_crc_errors = 3;
    EQ(sd_spi_dev.read(&sd, back, 10, 1), FS_EIO);
    c.read_crc_errors = 0;
    c.fault_sector = 13;
    c.read_token_errors = 3;
    EQ(sd_spi_dev.read(&sd, back, 10, 4), FS_EIO);
    c.read_token_errors = 0;
    EQ(sd_spi_dev.read(&sd, back, 10, 4), 0);   /* and the card is usable after */
    CHECK(!memcmp(back, buf, 512 * 4));
    /* writes: a damaged block is sent again, a failed one is an error */
    c.write_crc_errors = 1;
    memset(buf, 0x77, sizeof buf);
    EQ(sd_spi_dev.write(&sd, buf, 20, 1), 0);
    c.write_crc_errors = 1;
    EQ(sd_spi_dev.write(&sd, buf, 22, 4), 0);
    EQ(sd_spi_dev.read(&sd, back, 20, 1), 0);
    CHECK(!memcmp(back, buf, 512));
    EQ(sd_spi_dev.read(&sd, back, 22, 4), 0);
    CHECK(!memcmp(back, buf, 512 * 4));
    c.write_crc_errors = 5;
    EQ(sd_spi_dev.write(&sd, buf, 30, 1), FS_EIO);
    c.write_crc_errors = 0;
    c.write_failures = 1;
    EQ(sd_spi_dev.write(&sd, buf, 30, 2), FS_EIO);
    EQ(sd_spi_dev.write(&sd, buf, 30, 2), 0);
    EQ(c.mode, 0);
    EQ(c.bad_protocol, 0);   /* even after write errors the multiple block write was ended properly */
    /* a card that stays busy */
    c.stuck_busy = 1;
    EQ(sd_spi_dev.write(&sd, buf, 40, 1), FS_EIO);
    EQ(sd_spi_dev.sync(&sd), FS_EIO);
    free(c.img);
    /* a card that goes away after it was set up */
    make_card(&c, 2048, 1, 0);
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(sd_spi_dev.init(&sd), 0);
    c.no_card = 1;
    EQ(sd_spi_dev.read(&sd, back, 0, 1), FS_EIO);
    EQ(sd_spi_dev.write(&sd, buf, 0, 1), FS_EIO);
    c.no_card = 0;
    EQ(sd_spi_dev.read(&sd, back, 0, 1), 0);   /* it came back */
    free(c.img);
}

/* No card, through FatFs: the mount says so, not just "i/o error"; and a blank card says it has no file system. */
static void test_no_card_through_fatfs(void)
{
    card_t c;
    sd_spi_t sd;
    static fs_fat_t fat;
    make_card(&c, 65536, 1, 0);
    c.no_card = 1;
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(fs_fat_init(&fat, 0, &sd_spi_dev, &sd), FS_ENOMEDIA);
    EQ(fs_fat_format(0, &sd_spi_dev, &sd, FS_FAT_ANY), FS_ENOMEDIA);
    c.no_card = 0;
    EQ(fs_fat_init(&fat, 0, &sd_spi_dev, &sd), FS_ENODEV);   /* there, but blank */
    free(c.img);
}

/* The whole stack: FatFs on the driver on the card model, then fsck on what the card holds. */
static void test_stack(int sdhc, int v1, uint32_t sectors, int type)
{
    card_t c;
    sd_spi_t sd;
    static uint8_t data[40000], back[40000];
    char name[100];
    make_card(&c, sectors, sdhc, v1);
    sd_spi_setup(&sd, &io, &c, MAX_HZ);
    EQ(fs_fat_format(0, &sd_spi_dev, &sd, type), 0);
    static fs_fat_t fat;
    fs_reset();
    EQ(fs_fat_init(&fat, 0, &sd_spi_dev, &sd), 0);
    EQ(fs_mount("sd", &fs_fat_ops, &fat), 0);
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 13 + (i >> 8));
    EQ(fs_mkdir("/sd/Saves"), 0);
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof name, "/sd/Saves/Slot %d Game State.sav", i);
        const int fd = fs_open(name, FS_WRONLY | FS_CREAT);
        CHECK(fd >= 0);
        EQ(fs_write(fd, data, 1000u * (unsigned)i + 1), 1000 * i + 1);
        EQ(fs_close(fd), 0);
    }
    EQ(fs_write(fs_open("/sd/big.bin", FS_WRONLY | FS_CREAT), data, sizeof data), (int)sizeof data);
    EQ(fs_close(0), 0);
    EQ(fs_rename("/sd/Saves/Slot 3 Game State.sav", "/sd/Saves/renamed.sav"), 0);
    EQ(fs_unlink("/sd/Saves/Slot 5 Game State.sav"), 0);
    const int fd = fs_open("/sd/big.bin", FS_RDONLY);
    EQ(fs_read(fd, back, sizeof back), (int)sizeof back);
    CHECK(!memcmp(back, data, sizeof data));
    EQ(fs_close(fd), 0);
    fs_stat_t st;
    EQ(fs_stat("/sd/Saves/renamed.sav", &st), 0);
    EQ(st.size, 3001);
    fs_reset();
    EQ(fs_fat_done(&fat), 0);
    EQ(c.bad_crc_seen, 0);
    EQ(c.bad_protocol, 0);
    EQ(c.bad_speed_use, 0);
    /* what the card holds must be a sound file system */
    char path[300];
    snprintf(path, sizeof path, "%s/tilefs_sd_%d.img", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    FILE *fp = fopen(path, "wb");
    CHECK(fp && fwrite(c.img, 512, sectors, fp) == sectors);
    fclose(fp);
    const char *fsck = (type & ~FS_FAT_SFD) == FS_FAT_EXFAT ? "/usr/sbin/fsck.exfat" : "/usr/sbin/fsck.vfat";
    if (access(fsck, X_OK) == 0 && (type & FS_FAT_SFD)) {   /* fsck wants a bare file system: the SFD ones */
        char cmd[400];
        snprintf(cmd, sizeof cmd, "%s -n '%s' >/dev/null 2>&1", fsck, path);
        EQ(system(cmd), 0);
    }
    unlink(path);
    free(c.img);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_crc();
    test_init(16384, 1, 0, 9, 7);     /* a version 2 SDHC card: 8 MB */
    test_init(8192 * 1024, 1, 0, 9, 7);   /* 4 GB */
    test_init(16384, 0, 0, 9, 7);     /* a version 2 card that is byte addressed (an SDSC card that knows CMD8) */
    test_init(16384, 0, 1, 9, 7);     /* a version 1 card */
    test_init(16384, 0, 1, 10, 7);    /* a version 1 CSD with 1024-byte blocks */
    test_init(65536, 0, 1, 9, 3);     /* another C_SIZE_MULT */
    test_blocks(1, 0);
    test_blocks(0, 0);
    test_blocks(0, 1);
    test_faults();
    test_odd_cards();
    test_no_card_through_fatfs();
    printf("SD driver: %lu checks so far\n", checks);
    test_stack(1, 0, 131072, FS_FAT_FAT32 | FS_FAT_SFD);
    test_stack(1, 0, 131072, FS_FAT_FAT32);
    test_stack(0, 1, 32768, FS_FAT_FAT | FS_FAT_SFD);
    test_stack(1, 0, 65536, FS_FAT_EXFAT | FS_FAT_SFD);
    test_stack(0, 0, 65536, FS_FAT_EXFAT);
    printf("test_sd: %lu checks passed\n", checks);
    return 0;
}
