/*
 * rgb_led.h — small wrapper around the WS2812 PIO program.
 * Assumes a single WS2812 LED wired to a single GPIO (GP23 on YD-RP2040).
 */
#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>
#include <stdbool.h>

// Human-readable status states. rgb_led_set_state() maps each to a color.
typedef enum {
    RGB_STATE_BOOT,          // USB not yet enumerated — dim blue pulse
    RGB_STATE_READY,         // USB enumerated, waiting for RC frames — dim white
    RGB_STATE_ACTIVE,        // Frames arriving normally — green (brightness ~ frame rate)
    RGB_STATE_CENTERED,      // All sticks within ±2% of midpoint — bright green flash
    RGB_STATE_FAILSAFE,      // No frames > 500 ms — solid red
} rgb_state_t;

// Initialise the WS2812 on the given GPIO. Uses pio1 (leaves pio0 free for future SBUS).
void rgb_led_init(unsigned int gpio);

// Set the desired logical state. The internal update loop turns this into
// a color and drives the LED. Call from the main loop.
void rgb_led_set_state(rgb_state_t state);

// Must be called periodically (typ. every main-loop iteration) to advance
// pulse/flash animations. Non-blocking.
void rgb_led_task(void);

// Optional: drive a raw color directly (R/G/B 0..255). Bypasses state machine.
void rgb_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif /* RGB_LED_H */
