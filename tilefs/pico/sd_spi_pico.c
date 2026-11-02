/* SPDX-License-Identifier: MIT */
#include "sd_spi_pico.h"

#include "hardware/gpio.h"
#include "pico/time.h"

static void pico_cs(void *ctx, int asserted)
{
    const sd_spi_pico_t *p = ctx;
    gpio_put(p->cs, !asserted);
}

static void pico_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n)
{
    static const uint8_t ones[32] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    spi_inst_t *spi = ((const sd_spi_pico_t *)ctx)->spi;
    if (tx && rx) {
        spi_write_read_blocking(spi, tx, rx, n);
    } else if (tx) {
        spi_write_blocking(spi, tx, n);
    } else if (rx) {
        spi_read_blocking(spi, 0xFF, rx, n);
    } else {
        while (n) {
            const size_t k = n < sizeof ones ? n : sizeof ones;
            spi_write_blocking(spi, ones, k);
            n -= k;
        }
    }
}

static void pico_speed(void *ctx, uint32_t hz)
{
    spi_set_baudrate(((const sd_spi_pico_t *)ctx)->spi, hz);   /* the nearest rate that is not above hz */
}

static uint32_t pico_millis(void *ctx)
{
    (void)ctx;
    return to_ms_since_boot(get_absolute_time());
}

const sd_spi_io_t sd_spi_pico_io = {pico_cs, pico_xfer, pico_speed, pico_millis};

void sd_spi_pico_init(sd_spi_pico_t *port, sd_spi_t *sd, spi_inst_t *spi, uint miso, uint cs, uint sck, uint mosi, uint32_t max_hz)
{
    port->spi = spi;
    port->cs = (uint8_t)cs;
    spi_init(spi, SD_SPI_INIT_HZ);
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);   /* SD cards use mode 0 */
    gpio_set_function(sck, GPIO_FUNC_SPI);
    gpio_set_function(mosi, GPIO_FUNC_SPI);
    gpio_set_function(miso, GPIO_FUNC_SPI);
    gpio_pull_up(miso);   /* INPUT_PULLUP: an empty socket reads as 0xFF */
    gpio_init(cs);
    gpio_put(cs, 1);      /* not selected, before it is made an output */
    gpio_set_dir(cs, GPIO_OUT);
    sd_spi_setup(sd, &sd_spi_pico_io, port, max_hz);
}
