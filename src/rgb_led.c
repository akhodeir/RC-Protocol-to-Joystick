/*
 * rgb_led.c — WS2812 driver using PIO + a small color-state animation loop.
 *
 * Hardware: single WS2812 (GRB byte order) on GPIO 23 (YD-RP2040 on-board).
 * PIO: uses pio1 SM 0 so pio0 stays free for future SBUS work.
 */
#include "rgb_led.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

#define WS2812_FREQ_HZ     800000  // standard WS2812/WS2812B rate
#define WS2812_IS_RGBW     false

static PIO  s_pio       = pio1;
static uint s_sm        = 0;
static uint s_offset    = 0;
static bool s_inited    = false;

static rgb_state_t s_state = RGB_STATE_BOOT;

// Pack (R, G, B) into WS2812 GRB byte order, MSB-aligned in a 32-bit word.
// The PIO program consumes 24 bits per LED, MSB-first from the OSR.
static inline uint32_t pack_grb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
}

static inline void put_pixel(uint32_t grb_word) {
    if (!s_inited) return;
    pio_sm_put_blocking(s_pio, s_sm, grb_word);
}

void rgb_led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    put_pixel(pack_grb(r, g, b));
}

void rgb_led_init(unsigned int gpio) {
    s_offset = pio_add_program(s_pio, &ws2812_program);
    ws2812_program_init(s_pio, s_sm, s_offset, gpio, WS2812_FREQ_HZ, WS2812_IS_RGBW);
    s_inited = true;
    rgb_led_set_rgb(0, 0, 0);  // start dark
}

void rgb_led_set_state(rgb_state_t state) {
    s_state = state;
}

// Small time-based animation state.
static uint32_t s_last_ms       = 0;
static uint32_t s_flash_ms      = 0;    // when we entered CENTERED flash
static uint8_t  s_pulse_phase   = 0;    // 0..255 for the boot pulse

void rgb_led_task(void) {
    if (!s_inited) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((now - s_last_ms) < 20) return;   // 50 Hz update rate — fast enough for smooth pulse
    s_last_ms = now;

    switch (s_state) {
        case RGB_STATE_BOOT: {
            // Triangular pulse 0 → 40 → 0 on blue channel
            s_pulse_phase += 4;
            uint8_t v = s_pulse_phase < 128 ? s_pulse_phase / 4 : (255 - s_pulse_phase) / 4;
            rgb_led_set_rgb(0, 0, v);
            break;
        }
        case RGB_STATE_READY:
            rgb_led_set_rgb(6, 6, 6);   // dim white
            break;

        case RGB_STATE_ACTIVE:
            rgb_led_set_rgb(0, 20, 0);  // dim green — steady when frames arrive
            break;

        case RGB_STATE_CENTERED:
            if (s_flash_ms == 0) s_flash_ms = now;
            if (now - s_flash_ms < 200) {
                rgb_led_set_rgb(0, 120, 0);  // bright green flash
            } else if (now - s_flash_ms < 400) {
                rgb_led_set_rgb(0, 20, 0);   // back to dim green
            } else {
                s_flash_ms = 0;  // ready to flash again on re-entry
                s_state = RGB_STATE_ACTIVE;
            }
            break;

        case RGB_STATE_FAILSAFE:
            rgb_led_set_rgb(60, 0, 0);  // solid red
            break;
    }
}
