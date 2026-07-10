cmake_minimum_required(VERSION 3.13)

# ── Prefer the in-tree submodules for the SDK and picotool ────────────────
# The repo bundles Pico SDK and picotool under lib/ as git submodules, so a
# fresh clone (with --recurse-submodules) can build without any external
# installs. If the user already exported PICO_SDK_PATH / picotool_DIR
# pointing elsewhere, those take precedence.
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/pico-sdk"
        CACHE PATH "Path to the Pico SDK (defaults to in-tree submodule)")
    message(STATUS "Using bundled Pico SDK: ${PICO_SDK_PATH}")
endif()

# Redirect the SDK's FetchContent lookup for picotool to our in-tree copy so
# it doesn't try to git-clone raspberrypi/picotool during configure. The
# variable name matches CMake's FetchContent override convention.
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_PICOTOOL)
    set(FETCHCONTENT_SOURCE_DIR_PICOTOOL
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/picotool"
        CACHE PATH "Path to the picotool source (defaults to in-tree submodule)")
    message(STATUS "Using bundled picotool: ${FETCHCONTENT_SOURCE_DIR_PICOTOOL}")
    # The SDK's picotool version check runs first and errors out if the
    # system picotool is older than what the SDK requires. Force the SDK
    # to skip that check and use FetchContent (which will resolve to our
    # bundled copy via the override above).
    set(PICOTOOL_FORCE_FETCH_FROM_GIT ON
        CACHE BOOL "Use in-tree picotool submodule instead of any system install")
endif()

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
