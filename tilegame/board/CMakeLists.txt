cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350 CACHE STRING "")
# TinyUSB 0.18.0 with a patch for low-speed devices behind a hub (third_party/tinyusb/README-tilegame.md), unless PICO_TINYUSB_PATH is given.
option(USB_VENDORED_TINYUSB "Use the patched copy of TinyUSB in third_party/tinyusb instead of the Pico SDK's" ON)
set(USB_LS_GAP_US 300 CACHE STRING "Vendored TinyUSB: microseconds before each control transaction to a low-speed device behind a hub (0: off)")
if(USB_VENDORED_TINYUSB AND NOT PICO_TINYUSB_PATH AND NOT DEFINED ENV{PICO_TINYUSB_PATH})
    set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/../third_party/tinyusb CACHE PATH "" FORCE)
endif()
include(pico_sdk_import.cmake)
project(tilegame_board C CXX ASM)
pico_sdk_init()

set(CPU_MHZ 504 CACHE STRING "System clock in MHz: 126, 252, 378 or 504 (clk_hstx must come out at 126)")
set(TILEGFX_DIR ${CMAKE_CURRENT_LIST_DIR}/../../tilegfx CACHE PATH "The tilegfx compositor")
set(TILESND_DIR ${CMAKE_CURRENT_LIST_DIR}/../../tilesnd CACHE PATH "The tilesnd sound engine")
set(HSTX_FREE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../hstx-free CACHE PATH "hstx-free, which holds the upstream pico_hdmi")

# clk_hstx must be 126 MHz; the library divides clk_sys by this.
math(EXPR HSTX_DIV "${CPU_MHZ} / 126")
set(PICO_HDMI_HSTX_CLK_DIV ${HSTX_DIV} CACHE STRING "" FORCE)
add_subdirectory(${HSTX_FREE_DIR}/pico_hdmi pico_hdmi)

option(GAME_DEBUG_HUD "Show the frame rate and the USB task time under the HUD" OFF)
option(DEMO_AUTOPLAY "The demo presses buttons by itself" OFF)
option(PLAT_USB_HID "USB keyboards, mice and gamepads: the USB port becomes a host, so there is no USB serial console" OFF)
option(PLAT_USB_KEYBOARD "Old name for PLAT_USB_HID" OFF)
if(PLAT_USB_KEYBOARD)
    set(PLAT_USB_HID ON)
endif()
option(USBLOG "The USB event log, and the host recovery and panic guards that use its hooks (OFF: the USB driver as TinyUSB has it)" ON)
option(USB_PAD_LATE "XInput pad: open and poll its endpoints only once no device has appeared for 2.5 s" OFF)
option(TUSB_LOG "TinyUSB debug output (CFG_TUSB_DEBUG=2) into a RAM ring that the devkit USBLOG command saves as tusbNN.txt" OFF)
option(USB_PAUSE_POLL "With USBLOG: hold back the polling of other devices while a new one is being enumerated" OFF)
set(USB_PRE_DELAY_US 0 CACHE STRING "With USBLOG: wait (us) before a control transaction to a low-speed device behind a full-speed hub")
set(USB_FS_INT_MS 0 CACHE STRING "With USBLOG: the shortest polling interval (ms) given to a full-speed interrupt endpoint; 0: as the device asks")
option(USBLOG_TEST_PANIC "devkit: the STACK command panics, to try the USB log on the board" OFF)
option(DEMO_NO_MUSIC "With DEMO_AUTOPLAY: no jingle, so the effects can be heard alone" OFF)

function(tilegame_board_app name source)
    add_executable(${name} ${source} ${ARGN} plat_board.c ${TILEGFX_DIR}/tilegfx.c ${TILEGFX_DIR}/scene.c ${TILESND_DIR}/snd.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include ${CMAKE_CURRENT_LIST_DIR}/../game ${TILEGFX_DIR} ${TILESND_DIR})
    target_compile_definitions(${name} PRIVATE PLAT_CPU_MHZ=${CPU_MHZ} "TG_HOT_SECTION=\".time_critical.tilegfx\"" "SND_HOT_SECTION=\".time_critical.tilesnd\"")
    target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
    if(GAME_DEBUG_HUD)
        target_compile_definitions(${name} PRIVATE GAME_DEBUG_HUD)
    endif()
    if(DEMO_AUTOPLAY)
        target_compile_definitions(${name} PRIVATE DEMO_AUTOPLAY)
    endif()
    if(DEMO_NO_MUSIC)
        target_compile_definitions(${name} PRIVATE DEMO_NO_MUSIC)
    endif()
    target_link_libraries(${name} pico_stdlib pico_multicore hardware_vreg hardware_clocks hardware_dma
                          hardware_irq hardware_gpio hardware_sync pico_hdmi)
    if(PLAT_USB_HID)
        target_sources(${name} PRIVATE usb_hid.c xinput_host.c ${CMAKE_CURRENT_LIST_DIR}/../src/xinput.c ${CMAKE_CURRENT_LIST_DIR}/../src/hid_kbd.c ${CMAKE_CURRENT_LIST_DIR}/../src/keymap.c ${CMAKE_CURRENT_LIST_DIR}/../src/hid_pad.c
                       ${CMAKE_CURRENT_LIST_DIR}/../src/hid_mouse.c ${CMAKE_CURRENT_LIST_DIR}/../src/mouse_acc.c)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src)   # tusb_config.h, usb_hid.h and usblog.h
        target_compile_definitions(${name} PRIVATE PLAT_USB_HID=1 PLAT_STATS=0 $<$<BOOL:${USB_PAD_LATE}>:USB_PAD_LATE=1>)
        if(USBLOG)
            target_sources(${name} PRIVATE usblog_board.c usblog_usb.c $<$<BOOL:${TUSB_LOG}>:tusblog.c> ${CMAKE_CURRENT_LIST_DIR}/../src/usblog.c)
            target_compile_definitions(${name} PRIVATE CFG_TUH_RP2040_LS_GAP_US=${USB_LS_GAP_US} USBLOG=1 $<$<BOOL:${TUSB_LOG}>:TUSB_LOG=1> USB_PRE_DELAY_US=${USB_PRE_DELAY_US} USB_FS_INT_MS=${USB_FS_INT_MS} $<$<BOOL:${USB_PAUSE_POLL}>:USB_PAUSE_POLL=1> "USBLOG_HOT=__attribute__((section(\".time_critical.usblog\")))")
        endif()
        target_link_libraries(${name} tinyusb_host tinyusb_board)
        target_link_options(${name} PRIVATE -Wl,--wrap=panic -Wl,--wrap=hub_edpt_status_xfer)   # a panic in the USB interrupt handler is reported after the reset
        if(USBLOG)
            target_link_options(${name} PRIVATE -Wl,--wrap=hcd_event_handler -Wl,--wrap=hcd_edpt_xfer -Wl,--wrap=hcd_setup_send -Wl,--wrap=hcd_edpt_open
                -Wl,--wrap=hcd_port_reset -Wl,--wrap=hcd_port_reset_end -Wl,--wrap=hcd_device_close -Wl,--wrap=hcd_edpt_abort_xfer
                -Wl,--wrap=hcd_edpt_clear_stall -Wl,--wrap=hcd_port_speed_get -Wl,--wrap=hcd_int_disable -Wl,--wrap=hcd_int_enable)
        endif()
        pico_enable_stdio_usb(${name} 0)
    else()
        pico_enable_stdio_usb(${name} 1)
    endif()
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

tilegame_board_app(game_demo ${CMAKE_CURRENT_LIST_DIR}/../demo/game_demo.c)

set(GAME_DIR ${CMAKE_CURRENT_LIST_DIR}/../game)
tilegame_board_app(platformer ${GAME_DIR}/game_main.c ${GAME_DIR}/game.c ${GAME_DIR}/level.c ${GAME_DIR}/view.c
                   ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii8.c)

tilegame_board_app(padtest ${CMAKE_CURRENT_LIST_DIR}/../demo/padtest.c ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii8.c)

# Text mode demo: 80 x 25 text in the 640 x 480 picture (needs the 8x16 font for the PC character set).
set(DEMO_DIR ${CMAKE_CURRENT_LIST_DIR}/../demo)
tilegame_board_app(textdemo ${DEMO_DIR}/textdemo.c ${DEMO_DIR}/textdemo_pages.c ${TILEGFX_DIR}/tg_text.c ${TILEGFX_DIR}/tg_text_cp437.c
                   ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii16.c)
target_include_directories(textdemo PRIVATE ${DEMO_DIR})

# Console demo: type at a prompt on the 80 x 25 text screen (a USB keyboard), printf output.
tilegame_board_app(consdemo ${DEMO_DIR}/consdemo.c ${DEMO_DIR}/consdemo_logic.c ${TILEGFX_DIR}/tg_console.c ${TILEGFX_DIR}/tg_text.c
                   ${TILEGFX_DIR}/tg_text_cp437.c ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii16.c)
target_include_directories(consdemo PRIVATE ${DEMO_DIR})

# Editor demo: the editor of PicoParanoia on the 80 x 25 text screen, with the file in memory (a USB keyboard).
set(EDITOR_DIR ${CMAKE_CURRENT_LIST_DIR}/../editor)
tilegame_board_app(editdemo ${DEMO_DIR}/editdemo.c ${DEMO_DIR}/editdemo_logic.c ${EDITOR_DIR}/editor.c ${CMAKE_CURRENT_LIST_DIR}/../src/console_shim.c
                   ${CMAKE_CURRENT_LIST_DIR}/../src/console_platform.c ${TILEGFX_DIR}/tg_console.c ${TILEGFX_DIR}/tg_text.c
                   ${TILEGFX_DIR}/tg_text_cp437.c ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii16.c)
target_include_directories(editdemo PRIVATE ${DEMO_DIR} ${EDITOR_DIR})

# SD card test: FatFs on the SD socket (SPI0, GPIO4-7). A USB keyboard gives R (again) and F, Y (format). The file system code
# needs more stack than the 2 KB default: core 0 gets 4 KB, which is all its scratch bank holds (core 1 keeps 2 KB).
set(TILEFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../tilefs CACHE PATH "The tilefs file system layer")
include(${TILEFS_DIR}/tilefs.cmake)
tilegame_board_app(sdtest ${DEMO_DIR}/sdtest.c ${DEMO_DIR}/sdtest_logic.c ${TILEGFX_DIR}/tg_console.c ${TILEGFX_DIR}/tg_text.c
                   ${TILEGFX_DIR}/tg_text_cp437.c ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii16.c)
tilefs_add(sdtest)
target_include_directories(sdtest PRIVATE ${DEMO_DIR})
target_compile_definitions(sdtest PRIVATE SDTEST_BOARD=1 PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)

# File editor demo: the editor on files of the SD card (tilefs), RAM disk if there is no card. A USB keyboard is needed.
tilegame_board_app(editsd ${DEMO_DIR}/editsd.c ${DEMO_DIR}/editsd_logic.c ${EDITOR_DIR}/editor.c ${CMAKE_CURRENT_LIST_DIR}/../src/console_shim.c
                   ${CMAKE_CURRENT_LIST_DIR}/../src/console_platform.c ${TILEGFX_DIR}/tg_console.c ${TILEGFX_DIR}/tg_text.c
                   ${TILEGFX_DIR}/tg_text_cp437.c ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii16.c)
tilefs_add(editsd)
target_include_directories(editsd PRIVATE ${DEMO_DIR} ${EDITOR_DIR})
target_compile_definitions(editsd PRIVATE EDITSD_BOARD=1 PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)

# The development kit: BASIC (MY-BASIC, unmodified) and the editor (TDE) in one program, on the SD card. They run on a stack of their own (48 KB).
include(${CMAKE_CURRENT_LIST_DIR}/embed.cmake)
set(BASIC_DIR ${CMAKE_CURRENT_LIST_DIR}/../basic)
set(TDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../tde)
file(GLOB TDE_SOURCES ${TDE_DIR}/src/*.c)
list(FILTER TDE_SOURCES EXCLUDE REGEX "/menucua\\.c$")   # (the alternative menu, included by menu.c when asked for)
set(TDE_OS_SOURCES ${TDE_DIR}/os/console.c ${TDE_DIR}/os/port.c ${TDE_DIR}/os/criterr.c ${TDE_DIR}/os/tde_libc.c ${TDE_DIR}/os/tde_board.c)
embed_files(${CMAKE_CURRENT_BINARY_DIR}/devkit_config.c devkit_config_files ${TDE_DIR}/config tde.cfg tde.hlp tde.shl)
tilegame_board_app(devkit ${DEMO_DIR}/devkit.c ${CMAKE_CURRENT_LIST_DIR}/psram.c ${CMAKE_CURRENT_LIST_DIR}/../src/pool.c ${CMAKE_CURRENT_BINARY_DIR}/devkit_config.c ${BASIC_DIR}/basic_env.c ${BASIC_DIR}/my_basic/my_basic.c
                   ${TDE_SOURCES} ${TDE_OS_SOURCES} ${TILEGFX_DIR}/tg_console.c ${TILEGFX_DIR}/tg_text.c ${TILEGFX_DIR}/tg_text_cp437.c
                   ${TILEGFX_DIR}/tg_font.c ${TILEGFX_DIR}/fonts/unscii16.c)
tilefs_add(devkit)
target_include_directories(devkit PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src ${DEMO_DIR} ${BASIC_DIR} ${BASIC_DIR}/my_basic)
# TDE: built like its Unix version; char is signed (its allocator stores -1 in a char); the C library's file functions are the file layer's (tde_libc.c).
set_source_files_properties(${TDE_SOURCES} ${TDE_OS_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-fsigned-char;-DTDE_BOARD;-D__UNIX__;-D__TILEGAME__;-DNDEBUG;-I${CMAKE_CURRENT_LIST_DIR}/shim;-I${TDE_DIR}/src;-I${TDE_DIR}/os")
set_source_files_properties(${TDE_DIR}/src/main.c PROPERTIES COMPILE_OPTIONS "-w;-fsigned-char;-DTDE_BOARD;-D__UNIX__;-D__TILEGAME__;-DNDEBUG;-Dmain=tde_main;-I${CMAKE_CURRENT_LIST_DIR}/shim;-I${TDE_DIR}/src;-I${TDE_DIR}/os")
set_source_files_properties(${DEMO_DIR}/devkit.c PROPERTIES COMPILE_OPTIONS "-I${TDE_DIR}/os")
# MY-BASIC: the allocation tag is a size_t, so that its blocks are aligned; the packed structs it uses are fine on this core.
set_source_files_properties(${BASIC_DIR}/my_basic/my_basic.c ${BASIC_DIR}/basic_env.c PROPERTIES COMPILE_OPTIONS "-w;-Dmb_mem_tag_t=size_t")
target_link_options(devkit PRIVATE -Wl,--wrap=fopen,--wrap=stat,--wrap=chmod,--wrap=access,--wrap=utime,--wrap=unlink,--wrap=remove,--wrap=rmdir,--wrap=rename,--wrap=mkdir,--wrap=getcwd,--wrap=chdir,--wrap=realpath,--wrap=opendir,--wrap=readdir,--wrap=closedir)
target_compile_definitions(devkit PRIVATE DEVKIT_BOARD=1 PLAT_PSRAM=1 $<$<BOOL:${USBLOG_TEST_PANIC}>:USBLOG_TEST_PANIC=1> PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x800)
