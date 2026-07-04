/*
 * main.c — RC-Joystick firmware entry point.
 *
 * Reads FlySky iBUS on UART0 (GP1), maps 14 channels onto an 8-axis + 32-button
 * HID report, and blinks the on-board LED on GP25 to indicate status.
 *
 * Failsafe: if no valid frame is received for FAILSAFE_TIMEOUT_MS, all axes
 * center, throttle (Z) forces to minimum, and buttons clear.
 *
 * LED patterns on GP25:
 *   - Fast (100 ms) blink : USB not yet enumerated
 *   - Slow (500 ms) blink : USB enumerated, waiting for iBUS frames (or failsafe)
 *   - Solid on            : Frames arriving normally
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_descriptors.h"

#if defined(PROTOCOL_IBUS)
  #include "ibus.h"
  #include "hardware/uart.h"
  #define IBUS_UART     uart0
  #define RC_RX_PIN     1u        // GP1
  #define CH_MID        1500
  #define CH_HALF_RANGE 500
#else
  #error "This build requires PROTOCOL_IBUS. Use -DPROTOCOL=ibus."
#endif

#define STATUS_LED_PIN            25u    // YD-RP2040 on-board LED (Pico convention)
#define FAILSAFE_TIMEOUT_MS       500u

// ─────────────────────────────────────────────────────────────────────────────
// Channel → axis scaling (int16 signed, -32767..32767)
// ─────────────────────────────────────────────────────────────────────────────
static inline int16_t scale_axis(uint16_t raw) {
    int32_t v = (int32_t)raw - CH_MID;
    if (v >  CH_HALF_RANGE) v =  CH_HALF_RANGE;
    if (v < -CH_HALF_RANGE) v = -CH_HALF_RANGE;
    return (int16_t)(v * 32767 / CH_HALF_RANGE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fill a failsafe report — all centered, throttle to min, buttons clear.
// ─────────────────────────────────────────────────────────────────────────────
static void set_failsafe_report(joystick_report_t *r) {
    memset(r, 0, sizeof(*r));
    r->z = -32767;  // throttle bottomed
}

// ─────────────────────────────────────────────────────────────────────────────
// Status LED driver: pick a blink cadence based on current mode.
//   USB not mounted         → 100 ms fast blink
//   Mounted, no frames yet  → 500 ms slow blink
//   Mounted, frames active  → solid on
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    LED_MODE_BOOT,     // USB not enumerated
    LED_MODE_WAITING,  // USB up, no iBUS lock
    LED_MODE_ACTIVE,   // Frames arriving
} led_mode_t;

static void status_led_update(led_mode_t mode) {
    static uint32_t last_ms = 0;
    static bool on = false;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t period = 0;

    switch (mode) {
        case LED_MODE_BOOT:    period = 100; break;
        case LED_MODE_WAITING: period = 500; break;
        case LED_MODE_ACTIVE:
            if (!on) { on = true; gpio_put(STATUS_LED_PIN, 1); }
            return;
    }

    if ((now - last_ms) >= period) {
        last_ms = now;
        on = !on;
        gpio_put(STATUS_LED_PIN, on);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(void) {
    stdio_init_all();  // initialises clocks (harmless with stdio disabled)

    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_PIN, 0);

    ibus_init(IBUS_UART, RC_RX_PIN);
    tusb_init();

    joystick_report_t report;
    set_failsafe_report(&report);

    uint16_t channels[IBUS_NUM_CHANNELS] = {0};
    bool     rc_active     = false;
    uint32_t last_frame_ms = 0;
    uint32_t last_send_ms  = 0;

    while (true) {
        // 1. USB stack
        tud_task();

        // 2. Read iBUS
        bool new_frame = ibus_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (new_frame) {
            ibus_get_channels(channels, IBUS_NUM_CHANNELS);
            last_frame_ms = now;
            rc_active = true;
        }

        // 3. Failsafe timeout
        if (rc_active && (now - last_frame_ms) > FAILSAFE_TIMEOUT_MS) {
            rc_active = false;
        }

        // 4. Status LED
        led_mode_t mode = LED_MODE_BOOT;
        if (tud_mounted()) mode = rc_active ? LED_MODE_ACTIVE : LED_MODE_WAITING;
        status_led_update(mode);

        // 5. Build the HID report
        if (!rc_active) {
            set_failsafe_report(&report);
        } else {
            report.x      = scale_axis(channels[0]);
            report.y      = scale_axis(channels[1]);
            report.z      = scale_axis(channels[2]);
            report.rx     = scale_axis(channels[3]);
            report.ry     = scale_axis(channels[4]);
            report.rz     = scale_axis(channels[5]);
            report.slider = scale_axis(channels[6]);
            report.dial   = scale_axis(channels[7]);
            uint32_t btns = 0;
            for (int i = 8; i < IBUS_NUM_CHANNELS; i++) {
                if (channels[i] > CH_MID) btns |= (1u << (i - 8));
            }
            report.buttons = btns;
        }

        // 6. Send HID report at ~200 Hz max (5 ms).
        if ((now - last_send_ms) >= 5) {
            if (send_joystick_report(&report)) {
                last_send_ms = now;
            }
        }
    }
    return 0;
}
