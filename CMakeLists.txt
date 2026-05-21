cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(rp2040_4trigger_oem_sdk C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_4trigger_oem_sdk
    src/commands.c
    src/main.c
    src/settings.c
    src/status_led.c
    src/system_clock.c
    src/trigger.c
)

target_include_directories(rp2040_4trigger_oem_sdk PRIVATE
    src
)

target_link_libraries(rp2040_4trigger_oem_sdk
    pico_stdlib
    hardware_clocks
    hardware_flash
    hardware_gpio
    hardware_irq
    hardware_pwm
    hardware_vreg
)

pico_enable_stdio_usb(rp2040_4trigger_oem_sdk 1)
pico_enable_stdio_uart(rp2040_4trigger_oem_sdk 0)

# DO NOT call pico_add_extra_outputs() here.
# In Pico SDK 2.x, that can force CMake to build picotool/pioasm host tools.
# Your Windows install is failing because no host C/C++ compiler is visible.
#
# Instead, create UF2 using the prebuilt elf2uf2.exe from the Raspberry Pi Pico SDK installer.

set(ELF2UF2_CANDIDATES
    "$ENV{PICO_INSTALL_PATH}/pico-sdk-tools/elf2uf2.exe"
    "$ENV{PICO_INSTALL_PATH}/elf2uf2.exe"
    "${PICO_SDK_PATH}/../pico-sdk-tools/elf2uf2.exe"
    "${PICO_SDK_PATH}/../elf2uf2.exe"
    "C:/Program Files/Raspberry Pi/Pico SDK v2.2.0/pico-sdk-tools/elf2uf2.exe"
    "C:/Program Files/Raspberry Pi/Pico SDK v1.5.1/pico-sdk-tools/elf2uf2.exe"
)

set(ELF2UF2_EXE "")
foreach(candidate ${ELF2UF2_CANDIDATES})
    if(EXISTS "${candidate}")
        set(ELF2UF2_EXE "${candidate}")
        break()
    endif()
endforeach()

if(NOT ELF2UF2_EXE)
    message(FATAL_ERROR "Could not find elf2uf2.exe. Check your Pico SDK installation path.")
endif()

message(STATUS "Using elf2uf2: ${ELF2UF2_EXE}")

add_custom_command(TARGET rp2040_4trigger_oem_sdk POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:rp2040_4trigger_oem_sdk> ${CMAKE_CURRENT_BINARY_DIR}/rp2040_4trigger_oem_sdk.bin
    COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:rp2040_4trigger_oem_sdk> ${CMAKE_CURRENT_BINARY_DIR}/rp2040_4trigger_oem_sdk.hex
    COMMAND "${ELF2UF2_EXE}" $<TARGET_FILE:rp2040_4trigger_oem_sdk> ${CMAKE_CURRENT_BINARY_DIR}/rp2040_4trigger_oem_sdk.uf2
    COMMENT "Generating BIN, HEX, and UF2 without picotool"
)
