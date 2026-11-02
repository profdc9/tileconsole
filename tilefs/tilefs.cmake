# tilefs for a pico-sdk project: include() this file, then call tilefs_add(<target>), which adds the sources, the include paths and
# the libraries (the file system core, the RAM disk, FatFs with its glue, the SD card driver and its RP2040/RP2350 SPI transport).
get_filename_component(TILEFS_DIR ${CMAKE_CURRENT_LIST_FILE} DIRECTORY)

function(tilefs_add target)
    target_sources(${target} PRIVATE
        ${TILEFS_DIR}/tilefs.c ${TILEFS_DIR}/fs_ram.c ${TILEFS_DIR}/fs_blk.c ${TILEFS_DIR}/fs_fat.c ${TILEFS_DIR}/fs_fat_diskio.c
        ${TILEFS_DIR}/sd_spi.c ${TILEFS_DIR}/pico/sd_spi_pico.c
        ${TILEFS_DIR}/third_party/fatfs/ff.c ${TILEFS_DIR}/third_party/fatfs/ffunicode.c)
    target_include_directories(${target} PRIVATE ${TILEFS_DIR} ${TILEFS_DIR}/pico ${TILEFS_DIR}/third_party/fatfs)
    target_link_libraries(${target} hardware_spi hardware_gpio pico_time)
endfunction()
