cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350 CACHE STRING "")
include(pico_sdk_import.cmake)
project(hstx_free C CXX ASM)
pico_sdk_init()

set(CPU_MHZ 126 CACHE STRING "System clock in MHz: 126, 252, 378 or 504 (clk_hstx must come out at 126)")

# clk_hstx must be 126 MHz; the library divides clk_sys by this.
math(EXPR HSTX_DIV "${CPU_MHZ} / 126")
set(PICO_HDMI_HSTX_CLK_DIV ${HSTX_DIV} CACHE STRING "" FORCE)

# Upstream pico_hdmi (Unlicense), copied unmodified; see pico_hdmi/UPSTREAM_COMMIT.
add_subdirectory(pico_hdmi)
# The EmuBoard2350 works with the library default (CLK-/CLK+ on GPIO12/13, and likewise for each data pair).
# Turn this on only for a board that has the P side of each pair on the even GPIO.
option(SWAPPED_PAIRS "Board wires the P side of each HSTX pair to the even GPIO" OFF)

add_executable(hstx_free main.c)
target_compile_definitions(hstx_free PRIVATE CPU_MHZ=${CPU_MHZ})
if(SWAPPED_PAIRS)
    target_compile_definitions(hstx_free PRIVATE SWAPPED_PAIRS)
endif()
target_link_libraries(hstx_free pico_stdlib pico_multicore hardware_vreg hardware_clocks
                      hardware_dma hardware_irq hardware_gpio pico_hdmi)
pico_enable_stdio_usb(hstx_free 1)
pico_enable_stdio_uart(hstx_free 0)
pico_add_extra_outputs(hstx_free)
