/*
 * sbus.c — SBUS parser using hardware UART with pad-level polarity inversion.
 *
 * Strategy: sync on the 0x0F header, accumulate 25 bytes, verify the trailing
 * 0x00 footer, then unpack 16 × 11-bit channels from the 22 packed data bytes.
 * On footer mismatch, silently reset — next 0x0F resyncs.
 */
#include "sbus.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include <string.h>

static uart_inst_t *s_uart = NULL;

static uint8_t  s_buf[SBUS_FRAME_LEN];
static uint8_t  s_pos = 0;

static uint16_t s_channels[SBUS_NUM_CHANNELS];
static uint8_t  s_flags = 0;
static bool     s_lock = false;
static uint32_t s_last_byte_ms = 0;

void sbus_init(uart_inst_t *uart, unsigned int rx_pin) {
    s_uart = uart;
    uart_init(uart, 100000);
    uart_set_format(uart, 8, 2, UART_PARITY_EVEN);
    uart_set_fifo_enabled(uart, true);
    // SBUS is inverted; flip at the pad so the PL011 sees standard polarity.
    gpio_set_inover(rx_pin, GPIO_OVERRIDE_INVERT);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);
}

void sbus_deinit(uart_inst_t *uart, unsigned int rx_pin) {
    if (uart) uart_deinit(uart);
    gpio_set_inover(rx_pin, GPIO_OVERRIDE_NORMAL);
    s_uart = NULL;
}

void sbus_reset_state(void) {
    s_pos = 0;
    s_lock = false;
    memset(s_channels, 0, sizeof(s_channels));
    s_flags = 0;
    s_last_byte_ms = 0;
}

// Unpack 16 × 11-bit channels from 22 packed bytes, LSB-first.
// Sliding 3-byte window makes the bit-offset math straightforward.
static void unpack_channels(const uint8_t *d) {
    for (int ch = 0; ch < SBUS_NUM_CHANNELS; ch++) {
        int bit_pos  = ch * 11;
        int byte_idx = bit_pos / 8;
        int bit_off  = bit_pos % 8;
        uint32_t w = (uint32_t)d[byte_idx]
                   | ((uint32_t)d[byte_idx + 1] << 8)
                   | ((uint32_t)d[byte_idx + 2] << 16);
        s_channels[ch] = (uint16_t)((w >> bit_off) & 0x7FF);
    }
}

bool sbus_update(void) {
    if (!s_uart) return false;
    bool got_frame = false;

    while (uart_is_readable(s_uart)) {
        uint8_t b = uart_getc(s_uart);
        s_last_byte_ms = to_ms_since_boot(get_absolute_time());

        if (s_pos == 0) {
            if (b == SBUS_HEADER) s_buf[s_pos++] = b;
            // else discard until we sync on 0x0F
        } else {
            s_buf[s_pos++] = b;
            if (s_pos == SBUS_FRAME_LEN) {
                s_pos = 0;
                if (s_buf[24] == SBUS_FOOTER) {
                    unpack_channels(&s_buf[1]);
                    s_flags = s_buf[23];
                    s_lock = true;
                    got_frame = true;
                }
                // else: silently reset; next 0x0F resyncs
            }
        }
    }
    return got_frame;
}

bool sbus_get_channels(uint16_t *out, unsigned int n) {
    if (!s_lock) return false;
    if (n > SBUS_NUM_CHANNELS) n = SBUS_NUM_CHANNELS;
    for (unsigned int i = 0; i < n; i++) out[i] = s_channels[i];
    return true;
}

bool     sbus_has_lock(void)      { return s_lock; }
uint32_t sbus_last_byte_ms(void)  { return s_last_byte_ms; }
uint8_t  sbus_flags(void)         { return s_flags; }
