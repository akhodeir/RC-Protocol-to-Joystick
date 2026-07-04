/*
 * ibus.h — FlySky iBUS receiver on a Pico hardware UART.
 *
 * Protocol: 115200 8N1, non-inverted, single-wire from receiver signal pin.
 * Frame: 32 bytes, [0x20][0x40][14× ch_LE_16][csum_LE_16], every ~7 ms.
 * Checksum: 0xFFFF − sum(bytes[0..29]).
 */
#ifndef IBUS_H
#define IBUS_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/uart.h"

#define IBUS_NUM_CHANNELS   14
#define IBUS_FRAME_LEN      32
#define IBUS_HEADER_0       0x20
#define IBUS_HEADER_1       0x40

// Initialise iBUS on the given UART instance and RX GPIO. Receive-only.
void ibus_init(uart_inst_t *uart, unsigned int rx_pin);

// Drain any bytes waiting in the UART FIFO and advance the state machine.
// Returns true if a new valid frame was parsed on this call.
bool ibus_update(void);

// Copy the latest n channels (max IBUS_NUM_CHANNELS) into out[].
// Values are raw iBUS: 1000–2000. Returns false if no valid frame has arrived.
bool ibus_get_channels(uint16_t *out, uint n);

// True once at least one valid frame has been received.
bool ibus_has_lock(void);

// Timestamp (ms since boot) of the last raw byte read from the UART, regardless
// of whether it was part of a valid frame. Returns 0 if no byte ever arrived.
// Useful for distinguishing "no wire activity" from "bytes present but no valid
// iBUS frames" (e.g., wrong protocol or wiring).
uint32_t ibus_last_byte_ms(void);

#endif /* IBUS_H */
