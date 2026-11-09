cmake_minimum_required(VERSION 3.13)

# Pico W by default; pass -DPICO_BOARD=pico2_w for a Pico 2 W.
set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(vkc C CXX ASM)
set(CMAKE_C_STANDARD 11)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR "Pico SDK 2.0.0 or later is required")
endif()

pico_sdk_init()

add_executable(vkc
    main.c
    config.c
    net.c
    panel.c
    tape.c
    ${CMAKE_CURRENT_LIST_DIR}/../common/kcs.c
)

target_include_directories(vkc PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../common
)

target_compile_options(vkc PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(vkc
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_flash
    pico_unique_id
    hardware_flash
    hardware_gpio
    hardware_uart
    hardware_watchdog
)

# USB is the configuration console; the hardware UART is the command port.
pico_enable_stdio_usb(vkc 1)
pico_enable_stdio_uart(vkc 0)

pico_set_program_name(vkc "vkc")
pico_set_program_description(vkc "Kansas City Standard cassette emulator")
pico_add_extra_outputs(vkc)
