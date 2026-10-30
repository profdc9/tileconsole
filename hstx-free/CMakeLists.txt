cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350 CACHE STRING "")
include(pico_sdk_import.cmake)
project(hstx_free C CXX ASM)
pico_sdk_init()

# Upstream pico_hdmi (Unlicense), copied unmodified; see pico_hdmi/UPSTREAM_COMMIT.
add_subdirectory(pico_hdmi)

set(CPU_MHZ 126 CACHE STRING "System clock in MHz: 126, 252, 378 or 504 (clk_hstx must come out at 126)")
option(EMUBOARD2350 "EmuBoard2350 wiring: the P side of each pair is on the even GPIO" ON)

add_executable(hstx_free main.c)
target_compile_definitions(hstx_free PRIVATE CPU_MHZ=${CPU_MHZ})
if(EMUBOARD2350)
    target_compile_definitions(hstx_free PRIVATE EMUBOARD2350)
endif()
target_link_libraries(hstx_free pico_stdlib pico_multicore hardware_vreg hardware_clocks
                      hardware_dma hardware_irq hardware_gpio pico_hdmi)
pico_enable_stdio_usb(hstx_free 1)
pico_enable_stdio_uart(hstx_free 0)
pico_add_extra_outputs(hstx_free)
