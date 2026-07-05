/*
 * sbus.h — FrSky / Radiolink / FS-iA10B SBUS receiver via hardware UART.
 *
 * Protocol: 100000 baud, 8E2 (even parity, 2 stop bits), INVERTED signal.
 * The RP2040 PL011 UART is used directly; polarity is flipped at the pad
 * via gpio_set_inover(pin, GPIO_OVERRIDE_INVERT), so the peripheral sees
 * a standard-polarity UART frame — no PIO required.
 * Frame: 25 bytes, [0x0F][22 packed bytes = 16 × 11-bit channels][flags][0x00],
 * every ~14 ms.
 */
#ifndef SBUS_H
#define SBUS_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/uart.h"

#define SBUS_NUM_CHANNELS    16
#define SBUS_FRAME_LEN       25
#define SBUS_HEADER          0x0F
#define SBUS_FOOTER          0x00
#define SBUS_FLAG_FRAMELOST  0x04
#define SBUS_FLAG_FAILSAFE   0x08

// UART framing variant. Standard SBUS is 8E2, but some clone receivers
// emit 8N2 (no parity). Try 8E2 first; fall back to 8N2 in autodetect.
typedef enum {
    SBUS_PARITY_EVEN = 0,  // standard 8E2
    SBUS_PARITY_NONE = 1,  // relaxed 8N2 fallback
} sbus_parity_t;

// Initialise the UART for SBUS. Receive-only.
void sbus_init(uart_inst_t *uart, unsigned int rx_pin, sbus_parity_t parity);

// Tear down: deinit UART and clear the pad inversion. Call before switching
// to a different protocol on the same pin.
void sbus_deinit(uart_inst_t *uart, unsigned int rx_pin);

// Reset parser state (position, lock, channels, flags, last-byte timestamp).
void sbus_reset_state(void);

// Drain pending bytes; returns true if a valid frame was just completed.
bool sbus_update(void);

// Copy up to n channels (max 16) into out[]. Raw SBUS values: 172–1811.
bool sbus_get_channels(uint16_t *out, unsigned int n);

// True once at least one valid frame has been received.
bool sbus_has_lock(void);

// Timestamp (ms since boot) of the last raw byte from the UART; 0 if none.
uint32_t sbus_last_byte_ms(void);

// Latest flags byte (bit 2 = FRAMELOST, bit 3 = FAILSAFE).
uint8_t sbus_flags(void);

#endif /* SBUS_H */
