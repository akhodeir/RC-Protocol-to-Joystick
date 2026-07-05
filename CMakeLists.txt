cmake_minimum_required(VERSION 3.13)

# ── Pull in Pico SDK (must happen before project()) ───────────────────────
include(pico_sdk_import.cmake)

project(rc_joystick C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Original Raspberry Pi Pico (RP2040). Uses PICO_BOARD=pico by default.
# The firmware runs identically on other RP2040 boards (YD-RP2040 etc.)
# — GP1 for RC input, GP25 for status LED works on any of them.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico")
endif()

pico_sdk_init()

# ── Sources ────────────────────────────────────────────────────────────────
# Both iBUS and SBUS parsers are always compiled in; the firmware auto-detects
# which protocol is on the wire at boot.
add_executable(rc_joystick
    src/main.c
    src/usb_descriptors.c
    src/ibus.c
    src/sbus.c
)

# ── Include paths ──────────────────────────────────────────────────────────
target_include_directories(rc_joystick PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src    # tusb_config.h and our headers
)

# ── Link libraries ─────────────────────────────────────────────────────────
target_link_libraries(rc_joystick
    pico_stdlib
    hardware_uart
    tinyusb_device
    tinyusb_board
)

# ── stdio routing ──────────────────────────────────────────────────────────
# TinyUSB is managed manually. Disable Pico's stdio-USB to avoid clash.
# UART stdio is disabled because UART0 is used for RC signal reception.
pico_enable_stdio_usb(rc_joystick 0)
pico_enable_stdio_uart(rc_joystick 0)

# ── Output artifacts (.uf2, .hex, .bin, .map) ─────────────────────────────
pico_add_extra_outputs(rc_joystick)
