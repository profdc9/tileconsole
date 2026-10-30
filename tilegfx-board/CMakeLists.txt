cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350 CACHE STRING "")
include(pico_sdk_import.cmake)
project(tilegfx_board C CXX ASM)
pico_sdk_init()

set(CPU_MHZ 504 CACHE STRING "System clock in MHz: 126, 252, 378 or 504 (clk_hstx must come out at 126)")
set(TILEGFX_DIR ${CMAKE_CURRENT_LIST_DIR}/../tilegfx CACHE PATH "The tilegfx compositor")
set(HSTX_FREE_DIR ${CMAKE_CURRENT_LIST_DIR}/../hstx-free CACHE PATH "hstx-free, which holds the upstream pico_hdmi")

# clk_hstx must be 126 MHz; the library divides clk_sys by this.
math(EXPR HSTX_DIV "${CPU_MHZ} / 126")
set(PICO_HDMI_HSTX_CLK_DIV ${HSTX_DIV} CACHE STRING "" FORCE)
add_subdirectory(${HSTX_FREE_DIR}/pico_hdmi pico_hdmi)

add_executable(tilegfx_board
    main.c
    ${TILEGFX_DIR}/tilegfx.c
    ${TILEGFX_DIR}/scene.c)
target_include_directories(tilegfx_board PRIVATE ${TILEGFX_DIR})
target_compile_definitions(tilegfx_board PRIVATE CPU_MHZ=${CPU_MHZ} "TG_HOT_SECTION=\".time_critical.tilegfx\"")
target_compile_options(tilegfx_board PRIVATE -O2 -Wall -Wextra)
target_link_libraries(tilegfx_board pico_stdlib pico_multicore hardware_vreg hardware_clocks
                      hardware_dma hardware_irq hardware_gpio hardware_sync pico_hdmi)
pico_enable_stdio_usb(tilegfx_board 1)
pico_enable_stdio_uart(tilegfx_board 0)
pico_add_extra_outputs(tilegfx_board)
