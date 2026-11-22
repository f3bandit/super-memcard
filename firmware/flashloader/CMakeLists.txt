# Super MemCard's resident updater: pico-flashloader (Richard Hulme, BSD-3-Clause, github.com/rhulme/pico-flashloader
# 79817c8), unchanged, built beside the card firmware. Added by firmware/patches/0008 when SD2PSX_WITH_SUPERMEMCARD.
#
# Flash: the updater in the first 4 KB, the card firmware after it (linked at 0x10001000), a new firmware staged at
# 2 MB by the firmware update (src/ps1/ps1_supermemcard_update.c) for the updater to install at the next start.
# Outputs beside the firmware's own:
#   <target>.bin             the firmware for an update through the front port (built for the updater)
#   <target>-full.uf2        the updater and the firmware together: a card's first flash over USB (BOOTSEL)
# Copyright (C) 2026 f3bandit (this file).

set(SMC_LOADER smc_flashloader)
add_executable(${SMC_LOADER} ${CMAKE_CURRENT_SOURCE_DIR}/flashloader.c)
target_link_libraries(${SMC_LOADER} PRIVATE
        hardware_structs hardware_sync hardware_flash hardware_watchdog hardware_resets hardware_xosc hardware_clocks
        hardware_pll hardware_dma pico_platform pico_standard_link pico_crt0 pico_runtime_headers pico_divider)
pico_add_uf2_output(${SMC_LOADER})
pico_set_program_name(${SMC_LOADER} pico-flashloader)
pico_minimize_runtime(${SMC_LOADER} EXCLUDE ALL)
target_compile_options(${SMC_LOADER} PRIVATE -Wall -Wextra -Wno-ignored-qualifiers -Os)
target_link_directories(${SMC_LOADER} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
pico_set_linker_script(${SMC_LOADER} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_flashloader.ld)
pico_add_link_depend(${SMC_LOADER} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_defines.ld)
pico_add_link_depend(${SMC_LOADER} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_default.ld)

# the card firmware after the updater: sd2psXtd's own linker script with its flash moved up 4 KB (memmap_defines.ld's
# __APPLICATION_START), made here so the original stays as it is
file(READ ${CMAKE_SOURCE_DIR}/memmap_custom.ld SMC_LD)
set(SMC_LD_FROM "FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 2048k")
string(FIND "${SMC_LD}" "${SMC_LD_FROM}" SMC_LD_AT)
if (SMC_LD_AT LESS 0)
    message(FATAL_ERROR "flashloader: memmap_custom.ld's FLASH line changed: move the firmware up 4 KB by hand")
endif()
string(REPLACE "${SMC_LD_FROM}" "FLASH(rx) : ORIGIN = 0x10001000, LENGTH = 2044k" SMC_LD "${SMC_LD}")
file(WRITE ${CMAKE_BINARY_DIR}/memmap_smc_app.ld "${SMC_LD}")
set_target_properties(${TARGET_NAME} PROPERTIES PICO_TARGET_LINKER_SCRIPT ${CMAKE_BINARY_DIR}/memmap_smc_app.ld)

# the first flash over USB: the updater, then the firmware
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SMC_FULL_UF2 ${CMAKE_BINARY_DIR}/${TARGET_NAME}-full.uf2)
add_custom_command(OUTPUT ${SMC_FULL_UF2} DEPENDS ${SMC_LOADER} ${TARGET_NAME}
        COMMENT "Super MemCard: the updater and the firmware in one UF2"
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/uf2tool.py -o ${SMC_FULL_UF2}
                $<TARGET_FILE_DIR:${SMC_LOADER}>/${SMC_LOADER}.uf2 ${CMAKE_BINARY_DIR}/${TARGET_NAME}.uf2)
add_custom_target(smc_full_uf2 ALL DEPENDS ${SMC_FULL_UF2})
