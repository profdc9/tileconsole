cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350 CACHE STRING "")
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

option(DEMO_AUTOPLAY "The demo presses buttons by itself" OFF)
option(DEMO_NO_MUSIC "With DEMO_AUTOPLAY: no jingle, so the effects can be heard alone" OFF)

function(tilegame_board_app name source)
    add_executable(${name} ${source} plat_board.c ${TILEGFX_DIR}/tilegfx.c ${TILEGFX_DIR}/scene.c ${TILESND_DIR}/snd.c)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../include ${TILEGFX_DIR} ${TILESND_DIR})
    target_compile_definitions(${name} PRIVATE PLAT_CPU_MHZ=${CPU_MHZ} "TG_HOT_SECTION=\".time_critical.tilegfx\"" "SND_HOT_SECTION=\".time_critical.tilesnd\"")
    target_compile_options(${name} PRIVATE -O2 -Wall -Wextra)
    if(DEMO_AUTOPLAY)
        target_compile_definitions(${name} PRIVATE DEMO_AUTOPLAY)
    endif()
    if(DEMO_NO_MUSIC)
        target_compile_definitions(${name} PRIVATE DEMO_NO_MUSIC)
    endif()
    target_link_libraries(${name} pico_stdlib pico_multicore hardware_vreg hardware_clocks hardware_dma
                          hardware_irq hardware_gpio hardware_sync pico_hdmi)
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

tilegame_board_app(game_demo ${CMAKE_CURRENT_LIST_DIR}/../demo/game_demo.c)
