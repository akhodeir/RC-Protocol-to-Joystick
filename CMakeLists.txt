cmake_minimum_required(VERSION 3.13)

# ── Pull in Pico SDK (must happen before project()) ───────────────────────
include(pico_sdk_import.cmake)

project(rc_joystick C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# YD-RP2040 is not a first-class PICO_BOARD in the SDK; we use "pico" and
# declare our own pin assignments in code. USB is on-chip either way.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico")
endif()

pico_sdk_init()

# ── Protocol selection ─────────────────────────────────────────────────────
# Default to iBUS. SBUS is a future phase.
set(PROTOCOL "ibus" CACHE STRING "RC protocol: ibus (implemented) or sbus (deferred)")
string(TOLOWER "${PROTOCOL}" PROTOCOL)
message(STATUS "Building for protocol: ${PROTOCOL}")

# ── WS2812 pin override ────────────────────────────────────────────────────
# Default is GP23 (YD-RP2040 v1.4). Some clones/revisions use GP16, GP12,
# or GP25. Override on the CMake command line: -DWS2812_PIN=16
set(WS2812_PIN "23" CACHE STRING "GPIO number the WS2812 is wired to")
message(STATUS "WS2812 pin: GP${WS2812_PIN}")

# ── Sources ────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.c
    src/usb_descriptors.c
    src/rgb_led.c
)

if(PROTOCOL STREQUAL "sbus")
    message(FATAL_ERROR "SBUS support is deferred (see task #9 in the plan). Use -DPROTOCOL=ibus for now.")
elseif(PROTOCOL STREQUAL "ibus")
    list(APPEND SOURCES src/ibus.c)
    set(PROTOCOL_DEFINE "PROTOCOL_IBUS=1")
else()
    message(FATAL_ERROR "Unknown PROTOCOL '${PROTOCOL}'. Expected 'ibus' or 'sbus'.")
endif()

# ── Executable ─────────────────────────────────────────────────────────────
add_executable(rc_joystick ${SOURCES})

# ── PIO header generation (WS2812 always; SBUS later) ─────────────────────
pico_generate_pio_header(rc_joystick ${CMAKE_CURRENT_SOURCE_DIR}/src/ws2812.pio)

# ── Include paths ──────────────────────────────────────────────────────────
target_include_directories(rc_joystick PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}        # for tusb_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src    # for our headers
)

# ── Compile definitions ────────────────────────────────────────────────────
target_compile_definitions(rc_joystick PRIVATE
    ${PROTOCOL_DEFINE}
    WS2812_PIN_OVERRIDE=${WS2812_PIN}
)

# ── Link libraries ─────────────────────────────────────────────────────────
target_link_libraries(rc_joystick
    pico_stdlib
    hardware_uart
    hardware_pio
    hardware_dma
    tinyusb_device
    tinyusb_board
)

# ── stdio routing ──────────────────────────────────────────────────────────
# We manage USB ourselves via TinyUSB. Disable Pico's stdio-USB to avoid clash.
# UART stdio is disabled because UART0 is used for iBUS reception.
pico_enable_stdio_usb(rc_joystick 0)
pico_enable_stdio_uart(rc_joystick 0)

# ── Output artifacts (.uf2, .hex, .bin, .map) ─────────────────────────────
pico_add_extra_outputs(rc_joystick)
