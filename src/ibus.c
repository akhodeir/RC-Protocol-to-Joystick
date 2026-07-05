/*
 * ibus.c — state-machine iBUS parser.
 *
 * Strategy: hunt for the [0x20][0x40] header, accumulate 32 bytes,
 * validate checksum, then unpack 14 channels. On checksum mismatch,
 * silently resync at the next 0x20 byte. Inter-frame gap (~4 ms of the
 * ~7 ms period) is long enough that occasional desync auto-corrects.
 */
#include "ibus.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include <string.h>

static uart_inst_t *s_uart = NULL;

static uint8_t  s_buf[IBUS_FRAME_LEN];
static uint8_t  s_pos = 0;

static uint16_t s_channels[IBUS_NUM_CHANNELS];
static bool     s_lock = false;
static uint32_t s_last_byte_ms = 0;

void ibus_init(uart_inst_t *uart, unsigned int rx_pin) {
    s_uart = uart;
    uart_init(uart, 115200);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart, true);
    // Set function first (zeroes CTRL), then explicitly clear INOVER in case
    // a prior SBUS attempt left the pad inverted. Order matches sbus_init.
    gpio_set_function(rx_pin, GPIO_FUNC_UART);
    gpio_set_inover(rx_pin, GPIO_OVERRIDE_NORMAL);
    // Receive-only: no TX pin configured.
}

void ibus_deinit(uart_inst_t *uart, unsigned int rx_pin) {
    (void)rx_pin;
    if (uart) uart_deinit(uart);
    s_uart = NULL;
}

void ibus_reset_state(void) {
    s_pos = 0;
    s_lock = false;
    memset(s_channels, 0, sizeof(s_channels));
    s_last_byte_ms = 0;
}

static bool validate_checksum(const uint8_t *buf) {
    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) sum += buf[i];
    uint16_t expected = 0xFFFF - sum;
    uint16_t got = (uint16_t)(buf[30] | ((uint16_t)buf[31] << 8));
    return expected == got;
}

static void parse_channels(const uint8_t *buf) {
    for (int i = 0; i < IBUS_NUM_CHANNELS; i++) {
        s_channels[i] = (uint16_t)(buf[2 + i * 2] | ((uint16_t)buf[2 + i * 2 + 1] << 8));
    }
    s_lock = true;
}

bool ibus_update(void) {
    if (!s_uart) return false;
    bool got_frame = false;

    while (uart_is_readable(s_uart)) {
        uint8_t b = uart_getc(s_uart);
        s_last_byte_ms = to_ms_since_boot(get_absolute_time());

        if (s_pos == 0) {
            if (b == IBUS_HEADER_0) s_buf[s_pos++] = b;
            // else discard
        } else if (s_pos == 1) {
            if (b == IBUS_HEADER_1) {
                s_buf[s_pos++] = b;
            } else {
                // Not a valid pair — treat b as a potential new header start
                s_pos = 0;
                if (b == IBUS_HEADER_0) s_buf[s_pos++] = b;
            }
        } else {
            s_buf[s_pos++] = b;
            if (s_pos == IBUS_FRAME_LEN) {
                s_pos = 0;
                if (validate_checksum(s_buf)) {
                    parse_channels(s_buf);
                    got_frame = true;
                }
                // on bad checksum: state is reset, next 0x20 re-syncs
            }
        }
    }
    return got_frame;
}

bool ibus_get_channels(uint16_t *out, uint n) {
    if (!s_lock) return false;
    if (n > IBUS_NUM_CHANNELS) n = IBUS_NUM_CHANNELS;
    for (uint i = 0; i < n; i++) out[i] = s_channels[i];
    return true;
}

bool ibus_has_lock(void) {
    return s_lock;
}

uint32_t ibus_last_byte_ms(void) {
    return s_last_byte_ms;
}
