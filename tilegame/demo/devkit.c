/* SPDX-License-Identifier: MIT */
/*
 * The development kit on the board: the BASIC environment (MY-BASIC) and the editor (TDE) in one program, on the files of the SD card (/sd; a RAM disk if there is
 * no usable card). EDIT at the BASIC prompt runs TDE on the file; leaving TDE comes back to BASIC with the program loaded again. A USB keyboard is needed.
 *
 * Both want more stack than the 4 KB core 0 has (TDE keeps path-sized buffers in its functions), so everything runs on a stack of its own in ordinary RAM.
 *
 * On the desktop (no DEVKIT_BOARD) it is the same program: /sd is a folder of the host (the first argument, default ./devkit_files), the editor's configuration
 * comes from tde/config of the source tree the first time (run from the tilegame directory), and the window is SDL's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef DEVKIT_BOARD
#include "psram.h"
#include "pool.h"
#include "tdealloc.h"
#include "my_basic.h"
#if PLAT_USB_HID
#include "usblog_board.h"
#ifdef TUSB_LOG
#include "tusblog.h"
#endif
#endif
#include <malloc.h>
#include <unistd.h>
#endif

#include "basic_env.h"
#include "platform.h"
#include "tde_os.h"
#include "tg_text.h"
#include "tilefs.h"
#include "tilegfx.h"

#ifdef DEVKIT_BOARD
#include "embedded.h"
#include "fs_fat.h"
#include "sd_spi_pico.h"
#define SD_HZ 12500000u
#define STACK_BYTES (48 * 1024)
#else
#include <sys/stat.h>
#include <sys/types.h>
FILE *__real_fopen(const char *name, const char *mode);
int __real_mkdir(const char *path, mode_t mode);   /* the C library's own (the editor's fopen is answered from tilefs) */
#endif

static tg_text_t screen;
static tg_t g;
#ifdef DEVKIT_BOARD
static basic_env_t *env_p;   /* (in the PSRAM, with the interpreter's memory) */
#define env (*env_p)
#else
static basic_env_t env;
#endif
#ifdef DEVKIT_BOARD
static fs_fat_t fat;
static fs_ram_t ram;
#define RAM_DISK_BYTES (96 * 1024)
static bool psram_ok;
static uint8_t *ram_disk;
static size_t ram_disk_bytes;
static pool_t *pool;   /* the interpreter's and the editor's memory (the editor needs about 1 MB, and only while it runs) */
static sd_spi_pico_t port;
static sd_spi_t sd;
static uint8_t stack[STACK_BYTES] __attribute__((aligned(8)));
#else
static fs_posix_t host;
static const char *host_dir = "devkit_files";
#endif

#ifdef DEVKIT_BOARD
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"

/* What the previous run left in the watchdog scratch registers (they survive the reset): a hard fault (where) or the crumb of how far the editor got (a hang is
 * caught by the platform layer's monitor, which resets the board). Scratch 0 to 3 are the hang monitor's and the fault's, 5 is the crumb. */
#define FAULT_MAGIC 0x464C5421u   /* "FLT!" */
#define CRUMB_MAGIC 0xC4000000u

void tde_crumb(unsigned id)
{
    watchdog_hw->scratch[5] = CRUMB_MAGIC | id;
}

void devkit_fault(uint32_t *frame)
{
#if PLAT_USB_HID
    usblog(USBLOG_EV_FAULT, 0, frame[6], frame[5], *(volatile uint32_t *)0xE000ED28u, 0);
    usblog_close_run(USBLOG_CLOSED_FAULT);
#endif
    watchdog_hw->scratch[0] = FAULT_MAGIC;
    watchdog_hw->scratch[1] = frame[6];                              /* the instruction */
    watchdog_hw->scratch[2] = frame[5];                              /* the caller */
    watchdog_hw->scratch[3] = *(volatile uint32_t *)0xE000ED28u;     /* the fault status (CFSR) */
    watchdog_hw->scratch[6] = *(volatile uint32_t *)0xE000ED38u;     /* the bus fault address (BFAR) */
    watchdog_hw->scratch[7] = frame[0];
    watchdog_reboot(0, 0, 0);
    for (;;) {}
}

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile("tst lr, #4\n\tite eq\n\tmrseq r0, msp\n\tmrsne r0, psp\n\tb devkit_fault\n");
}

/* A line for the first screen about how the previous run ended, if it did not end by itself; clears what it read. */
static const char *last_run_note(void)
{
    static char text[200];
    const uint32_t magic = watchdog_hw->scratch[0], crumb = watchdog_hw->scratch[5];
    text[0] = 0;
    if (magic == FAULT_MAGIC)
        snprintf(text, sizeof text, "Last run: HARD FAULT at %08lX from %08lX, CFSR %08lX BFAR %08lX R0 %08lX, after step %lu.\n", (unsigned long)watchdog_hw->scratch[1],
                 (unsigned long)watchdog_hw->scratch[2], (unsigned long)watchdog_hw->scratch[3], (unsigned long)watchdog_hw->scratch[6], (unsigned long)watchdog_hw->scratch[7],
                 (unsigned long)(crumb & 0xFFFF));
    else if ((crumb & 0xFF000000u) == CRUMB_MAGIC && (magic == 0x48414E47u || magic == 0x50414E49u))
        snprintf(text, sizeof text, "Last run: %s, after step %lu.\n", magic == 0x48414E47u ? "HUNG" : "PANIC", (unsigned long)(crumb & 0xFFFF));
    watchdog_hw->scratch[5] = 0;
    if (magic == FAULT_MAGIC)
        watchdog_hw->scratch[0] = 0;
    return text[0] ? text : NULL;
}
#endif

static void show(void)
{
    plat_present(&g);
}

static void frame(void)
{
    plat_wait_frame();
    plat_present(&g);
}

static void idle(void)
{
    frame();
}

#ifdef DEVKIT_BOARD
/* Mounts what holds the files at /sd; returns a line for the first screen if it is not the card. */
static const char *mount_files(void)
{
    sd_spi_pico_init(&port, &sd, spi0, SD_SPI_PICO_EMUBOARD2350, SD_HZ);
    const int e = fs_fat_init(&fat, 0, &sd_spi_dev, &sd);
    if (e == 0 && fs_mount("sd", &fs_fat_ops, &fat) == 0)
        return NULL;
    static char why[100];
    snprintf(why, sizeof why, "No usable SD card (%s): the files are in RAM and are lost at power off.\n", fs_strerror(e < 0 ? e : FS_EIO));
    fs_fat_done(&fat);
    if (ram_disk)
        fs_ram_init(&ram, ram_disk, ram_disk_bytes);
    else if ((ram_disk = malloc(RAM_DISK_BYTES)) != NULL)   /* (no PSRAM: taken from the heap, and only when it is used) */
        fs_ram_init(&ram, ram_disk, RAM_DISK_BYTES);
    fs_mount("sd", &fs_ram_ops, &ram);
    return why;
}

#if PLAT_USB_HID && USBLOG && defined(DEVKIT_BOARD)
static void write_chunk(void *ctx, const void *data, size_t size)
{
    int *fd = ctx;
    if (*fd >= 0 && fs_write(*fd, data, size) != (int)size)
        *fd = -1;
}

/* Writes a USB log to the next free /sd/usblogNN.bin (tools/usblog_decode.py reads it); returns the path, or NULL if it could not be written. */
static const char *write_usblog(const usblog_t *ring)
{
    static char path[32];
    fs_stat_t st;
    int n = 0;
    while (n < 100) {
        snprintf(path, sizeof path, "/sd/usblog%02d.bin", n);
        if (fs_stat(path, &st) != 0)
            break;
        n++;
    }
    const int fd = n < 100 ? fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC) : -1;
    int out = fd;
    if (fd >= 0) {
        usblog_serialize(ring, write_chunk, &out);
        fs_close(fd);
    }
#ifdef TUSB_LOG
    if (fd >= 0 && out >= 0) {   /* TinyUSB's own debug text, as /sd/tusbNN.txt with the same number */
        char tpath[32];
        snprintf(tpath, sizeof tpath, "/sd/tusb%02d.txt", n);
        const int tfd = fs_open(tpath, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (tfd >= 0) {
            static char buf[512];
            size_t off = 0, got;
            while ((got = tusblog_read(off, buf, sizeof buf)) > 0 && fs_write(tfd, buf, got) == (int)got)
                off += got;
            fs_close(tfd);
        }
    }
#endif
    return fd >= 0 && out >= 0 ? path : NULL;
}

/* If the previous run ended in a hang, a panic or a fault, its USB log is saved; returns a line for the screen. */
static const char *save_usblog(void)
{
    const usblog_t *old = usblog_saved();
    if (!old)
        return NULL;
    static char text[160];
    static const char *const how[] = {"?", "hung", "panicked", "faulted"};
    const char *why = old->closed < 4 ? how[old->closed] : how[0];
    const char *path = write_usblog(old);
    if (path)
        snprintf(text, sizeof text, "The last run %s: its %u USB events are in %s.\n", why, (unsigned)usblog_count(old), path);
    else
        snprintf(text, sizeof text, "The last run %s, and its USB log could not be saved.\n", why);
    usblog_saved_free();
    return text;
}
#endif

/* The editor's configuration into /sd/tde the first time (it looks there: TDEHOME). */
static void install_config(void)
{
    fs_stat_t st;
    if (fs_stat("/sd/tde/tde.cfg", &st) == 0)
        return;
    fs_mkdir("/sd/tde");
    for (const embedded_file_t *f = devkit_config_files; f->name; f++) {
        char path[64];
        snprintf(path, sizeof path, "/sd/tde/%s", f->name);
        const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (fd >= 0) {
            fs_write(fd, f->data, f->size);
            fs_close(fd);
        }
    }
}
#else
static const char *mount_files(void)
{
    __real_mkdir(host_dir, 0777);
    if (fs_posix_init(&host, host_dir) != 0 || fs_mount("sd", &fs_posix_ops, &host) != 0) {
        static char why[200];
        snprintf(why, sizeof why, "Cannot use the folder %s.\n", host_dir);
        return why;
    }
    return NULL;
}

static void install_config(void)
{
    static const char *const names[] = {"tde.cfg", "tde.hlp", "tde.shl"};
    static char data[100000];
    fs_stat_t st;
    if (fs_stat("/sd/tde/tde.cfg", &st) == 0)
        return;
    fs_mkdir("/sd/tde");
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char host_path[100], path[100];
        snprintf(host_path, sizeof host_path, "tde/config/%s", names[i]);
        snprintf(path, sizeof path, "/sd/tde/%s", names[i]);
        FILE *fp = __real_fopen(host_path, "rb");
        if (!fp) {
            fprintf(stderr, "no %s: the editor starts without its configuration files\n", host_path);
            return;
        }
        const size_t n = fread(data, 1, sizeof data, fp);
        fclose(fp);
        const int fd = fs_open(path, FS_WRONLY | FS_CREAT | FS_TRUNC);
        if (fd >= 0) {
            fs_write(fd, data, n);
            fs_close(fd);
        }
    }
}
#endif

static int get_char(void)
{
#ifndef DEVKIT_BOARD
    if (plat_quit_requested())
        exit(0);
#endif
    return plat_getchar();
}

/* The two ways of reading the keyboard (typed characters, key presses) each keep what the other one was given: when one program hands over to the other, what is
 * still queued is what was typed to the first one, and is thrown away. */
static void drain_keys(void)
{
    plat_key_event_t ev;
    while (plat_getkey(&ev)) {}
    while (plat_getchar() >= 0) {}
}

#ifndef DEVKIT_BOARD
void tde_crumb(unsigned id) { (void)id; }
#endif

static void run_editor(const char *name)
{
    static char program[] = "/sd/tde/tde";
    static char file[BASIC_NAME_MAX + 5];
    char *args[3] = {program, NULL, NULL};
    int n = 1;
    snprintf(file, sizeof file, "%s", name);
    if (*file)
        args[n++] = file;
    tde_console_attach(&screen, show);
    tde_console_set_idle(idle);
    setenv("TDEHOME", "/sd/tde", 1);
    drain_keys();
    tde_crumb(10);
    tde_main(n, args);
    tde_crumb(11);
    drain_keys();
}

#ifdef DEVKIT_BOARD
static void *tde_get(size_t n) { return pool_malloc(pool, n); }
static void *tde_resize(void *p, size_t n) { return pool_realloc(pool, p, n); }
static void tde_put(void *p) { pool_free(pool, p); }
static long tde_avail(void) { return (long)pool_free_bytes(pool); }
static char *basic_get(unsigned n) { return pool_malloc(pool, n); }
static void basic_put(char *p) { pool_free(pool, p); }

/* The PSRAM, after the self-test has used it: a quarter is the RAM disk (when there is no card), the rest is one pool for the
 * interpreter (its programs and the BASIC environment) and the editor. Without PSRAM, everything stays in the C library's heap. */
static void setup_memory(void)
{
    static const tde_allocator_t from_pool = {tde_get, tde_resize, tde_put, tde_avail};
    const size_t size = psram_ok ? psram_size() : 0;
    if (size >= (1u << 20)) {
        uint8_t *base = PSRAM_BASE;
        const size_t quarter = size / 4;
        ram_disk = base;
        ram_disk_bytes = quarter;
        pool = pool_create(base + quarter, size - quarter);
        if (pool) {
            mb_set_memory_manager(basic_get, basic_put);
            tde_state_set_allocator(&from_pool);
        }
    }
    env_p = pool ? pool_calloc(pool, 1, sizeof *env_p) : calloc(1, sizeof *env_p);
    if (!env_p)
        for (;;) {}
}

#define STACK_FILL 0xA5

/* the most the stack has been used: the lowest byte that is no longer the fill main() painted */
static size_t stack_peak(void)
{
    size_t free_bytes = 0;
    while (free_bytes < sizeof stack && stack[free_bytes] == STACK_FILL)
        free_bytes++;
    return sizeof stack - free_bytes;
}

extern char __HeapLimit;

/* STACK and HEAP at the BASIC prompt */
static int board_info(const char *what, char *out, size_t size)
{
    if (!strcmp(what, "stack")) {
#ifdef USBLOG_TEST_PANIC
        panic("usblog test %d\n", 42);   /* (a test build: the USB log must survive this and be saved at the next start) */
#endif
        snprintf(out, size, "stack: peak %u of %u bytes used", (unsigned)stack_peak(), (unsigned)sizeof stack);
        return 1;
    }
#if PLAT_USB_HID && USBLOG
    if (!strcmp(what, "usblog")) {   /* the log as it is now, for a problem that does not end in a reset */
        const usblog_t *live = usblog_live();
        const char *path = write_usblog(live);
        if (path)
            snprintf(out, size, "USB log: %u events saved in %s", (unsigned)usblog_count(live), path);
        else
            snprintf(out, size, "USB log: could not be saved");
        return 1;
    }
#endif
    if (!strcmp(what, "heap")) {
        const struct mallinfo mi = mallinfo();
        const long system = (long)mi.fordblks + (long)(&__HeapLimit - (char *)sbrk(0));
        if (pool)
            snprintf(out, size, "heap: %ld bytes free in the PSRAM pool, %ld in the system heap", (long)pool_free_bytes(pool), system);
        else
            snprintf(out, size, "heap: %ld bytes free in the system heap", system);
        return 1;
    }
    return 0;
}

#endif

static void devkit(void)
{
    #ifdef DEVKIT_BOARD
    static const basic_hooks_t hooks = {get_char, frame, plat_millis, board_info};
#else
    static const basic_hooks_t hooks = {get_char, frame, plat_millis, NULL};
#endif
#ifdef DEVKIT_BOARD
    static char note_copy[200];
    const char *last = last_run_note();   /* (before plat_init, which clears the hang report) */
    if (last)
        snprintf(note_copy, sizeof note_copy, "%s", last);
#endif
    if (!plat_init()) {
#ifdef DEVKIT_BOARD
        for (;;) {}
#else
        exit(1);
#endif
    }
    tg_text_load_cp437(&screen);
    tg_text_init(&screen);
    tg_init(&g);
    g.mode = TG_MODE_TEXT;
    g.text = &screen;
#ifdef DEVKIT_BOARD
    char psram_note[120];
    {
        const size_t sz = psram_size();
        if (sz) {
            uint32_t w = 0, r = 0;
            const uint32_t bad = psram_selftest(1, &w, &r);
            snprintf(psram_note, sizeof psram_note, "PSRAM: %u MB, %s (%u bad), write %u KB/s, read %u KB/s\n", (unsigned)(sz >> 20),
                     bad ? "FAILED" : "ok", (unsigned)bad, (unsigned)w, (unsigned)r);
            psram_ok = bad == 0;   /* (a memory that does not hold what is written to it is not used) */
        } else {
            snprintf(psram_note, sizeof psram_note, "PSRAM: none found\n");
        }
    }
    setup_memory();
#endif
    const char *note = mount_files();
    fs_chdir("/sd");
    install_config();
#if PLAT_USB_HID && USBLOG && defined(DEVKIT_BOARD)
    const char *usb_note = save_usblog();
#endif
    basic_env_init(&env, &screen, &hooks);
#if PLAT_USB_HID && USBLOG && defined(DEVKIT_BOARD)
    if (usb_note)
        tg_console_puts(&env.con, usb_note);
#endif
    if (note)
        tg_console_puts(&env.con, note);
#ifdef DEVKIT_BOARD
    if (note_copy[0])
        tg_console_puts(&env.con, note_copy);
#endif
#ifdef DEVKIT_BOARD
    tg_console_puts(&env.con, psram_note);
#endif
    for (;;) {
        const int r = basic_env_run(&env);
        if (r == BASIC_EXIT_EDIT) {
            run_editor(env.edit_name);
            if (basic_env_load(&env, env.edit_name) < 0)
                env.length = 0;
            tg_console_puts(&env.con, "\n");
        } else {
#ifdef DEVKIT_BOARD
            char stack_line[100];
            board_info("stack", stack_line, sizeof stack_line);
            tg_console_puts(&env.con, stack_line);
            tg_console_puts(&env.con, "\n");
            tg_console_puts(&env.con, "Nothing to leave to: this is the whole machine. EDIT starts the editor.\n");
#else
            break;
#endif
        }
    }
}

int main(int argc, char **argv)
{
#ifdef DEVKIT_BOARD
    (void)argc;
    (void)argv;
    /* the stack grows down from the top of the array; interrupt handlers use it too */
    uint8_t *top = stack + sizeof stack;
    memset(stack, STACK_FILL, sizeof stack);   /* (main is still on the boot stack) */
    __asm volatile("msr msp, %0\n\tbx %1\n" ::"r"(top), "r"(devkit) : "memory");
    __builtin_unreachable();
#else
    if (argc > 1)
        host_dir = argv[1];
    devkit();
    basic_env_close(&env);
    plat_shutdown();
    return 0;
#endif
}
