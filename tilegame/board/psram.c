/* The Emu2350's PSRAM (an APS6404L-class QSPI part on QMI chip select 1). Set up the way the RP2350 datasheet describes: enter
 * direct mode, send "quad mode" (0x35), then give the memory-mapped window quad read (0xEB) and quad write (0x38) formats. */
#include "psram.h"

#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define XIP_CS1_FUNC 9u
#define NOCACHE_ALIAS 0x15000000u   /* the same memory, past the XIP cache: for finding its size */

static size_t found_size;

/* GPIO 32 and up are not in the SDK's tables when the board is a pico2 (RP2350A), so the pin's two registers are written directly:
 * the pad (isolation off, input enabled) and the function select. */
static void select_cs_pin(unsigned pin)
{
    volatile uint32_t *pad = (volatile uint32_t *)(PADS_BANK0_BASE + 0x04 + 4 * pin);
    volatile uint32_t *ctrl = (volatile uint32_t *)(IO_BANK0_BASE + 0x04 + 8 * pin);
    *pad = (*pad & ~((1u << 8) | (1u << 7))) | (1u << 6);   /* ISO off, OD off, IE on */
    *ctrl = XIP_CS1_FUNC;
}

static void __no_inline_not_in_flash_func(start_qmi)(unsigned max_mhz)
{
    const uint32_t clock_hz = clock_get_hz(clk_sys);
    qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}

    uint32_t div = (clock_hz + max_mhz * 1000000u - 1) / (max_mhz * 1000000u);
    if (div < 2 && clock_hz > 100000000u)
        div = 2;
    uint32_t rxdelay = div;
    if (clock_hz / div > 100000000u)
        rxdelay++;
    const uint32_t period_fs = (uint32_t)(1000000000000000ull / clock_hz);
    const uint32_t max_select = 125000000u / period_fs;                                  /* chip select low at most 8 us, in units of 64 clocks */
    const uint32_t min_deselect = (18000000u + period_fs - 1) / period_fs - (div + 1) / 2;   /* 18 ns high between accesses */

    qmi_hw->m[1].timing = 1u << QMI_M1_TIMING_COOLDOWN_LSB | QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB | min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB | div << QMI_M1_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB | QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
                        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB | QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
                        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB | QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
                        6u << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;
    qmi_hw->m[1].wfmt = QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB | QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
                        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB | QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
                        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB | QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;
    qmi_hw->direct_csr = 0;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

/* A smaller part repeats itself at its own size: write at 0 and at 1, 2, 4, 8 MB and see which one shows up at 0. */
static size_t find_size(void)
{
    volatile uint32_t *base = (volatile uint32_t *)NOCACHE_ALIAS;
    const uint32_t a = 0x13579BDFu, b = 0x2468ACE0u;
    uint32_t keep = base[0];
    base[0] = a;
    __dmb();
    if (base[0] != a) {
        base[0] = keep;
        return 0;
    }
    base[0] = keep;
    for (size_t bytes = 1u << 20; bytes <= 8u << 20; bytes <<= 1) {
        volatile uint32_t *probe = (volatile uint32_t *)(NOCACHE_ALIAS + bytes);
        keep = base[0];
        const uint32_t keep_probe = *probe;
        base[0] = a;
        __dmb();
        *probe = b;
        __dmb();
        const bool alias = base[0] == b;
        *probe = keep_probe;
        base[0] = keep;
        __dmb();
        if (alias)
            return bytes;
    }
    return 16u << 20;
}

size_t psram_init(unsigned cs_pin, unsigned max_mhz)
{
    select_cs_pin(cs_pin);
    start_qmi(max_mhz);
    found_size = find_size();
    return found_size;
}

size_t psram_size(void)
{
    return found_size;
}

uint32_t psram_selftest(int passes, uint32_t *write_kbs, uint32_t *read_kbs)
{
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_BASE;
    const size_t words = found_size / 4;
    uint32_t bad = 0;
    uint64_t write_us = 0, read_us = 0;
    for (int pass = 0; pass < passes; pass++) {
        const uint32_t seed = 0x9E3779B9u * (uint32_t)(pass + 1);
        uint64_t t = time_us_64();
        for (size_t i = 0; i < words; i++)
            p[i] = (uint32_t)i * 2654435761u ^ seed;
        write_us += time_us_64() - t;
        t = time_us_64();
        for (size_t i = 0; i < words; i++)
            if (p[i] != ((uint32_t)i * 2654435761u ^ seed))
                bad++;
        read_us += time_us_64() - t;
    }
    const uint64_t kb = (uint64_t)found_size / 1024 * (uint64_t)passes;
    if (write_kbs)
        *write_kbs = write_us ? (uint32_t)(kb * 1000000u / write_us) : 0;
    if (read_kbs)
        *read_kbs = read_us ? (uint32_t)(kb * 1000000u / read_us) : 0;
    return bad;
}
