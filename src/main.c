/*
 * main.c — RC-Joystick firmware entry point.
 *
 * Reads FlySky iBUS on UART0 (GP1), maps 14 channels onto an 8-axis + 32-button
 * HID report, and drives a WS2812 status LED on GP23 (YD-RP2040 on-board).
 *
 * Failsafe: if no valid frame is received for FAILSAFE_TIMEOUT_MS, all axes
 * center, throttle (Z) forces to minimum, and buttons clear. The LED goes red.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_descriptors.h"
#include "rgb_led.h"

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

#define RGB_LED_PIN               WS2812_PIN_OVERRIDE
#define HEARTBEAT_LED_PIN         25u    // YD-RP2040 on-board green LED (Pico convention)
#define FAILSAFE_TIMEOUT_MS       500u
#define CENTER_TOLERANCE          40   // ~2% of ±32767 → within ±640 counts feels tight;
                                       // apply against raw channel diff instead (see below)

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
// Are all four primary sticks within ±2% of center? Uses raw iBUS values so
// the tolerance is protocol-natural (~10 counts on a 1000-wide range).
// ─────────────────────────────────────────────────────────────────────────────
static bool sticks_centered(const uint16_t *ch) {
    const int TOL = 20;  // ±20 counts on a 1000-count half-range = 2 %
    for (int i = 0; i < 4; i++) {
        int diff = (int)ch[i] - CH_MID;
        if (diff < -TOL || diff > TOL) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(void) {
    stdio_init_all();  // initialises clocks (harmless with stdio disabled)

    // ── Heartbeat LED on GP25 (YD-RP2040 on-board green LED) ──────────────
    // Independent of the WS2812 driver; if this blinks, firmware is alive.
    gpio_init(HEARTBEAT_LED_PIN);
    gpio_set_dir(HEARTBEAT_LED_PIN, GPIO_OUT);
    gpio_put(HEARTBEAT_LED_PIN, 1);

    rgb_led_init(RGB_LED_PIN);
    rgb_led_set_state(RGB_STATE_BOOT);

    ibus_init(IBUS_UART, RC_RX_PIN);
    tusb_init();

    joystick_report_t report;
    set_failsafe_report(&report);

    uint16_t channels[IBUS_NUM_CHANNELS] = {0};
    bool     rc_active     = false;
    uint32_t last_frame_ms = 0;
    uint32_t last_send_ms  = 0;
    uint32_t last_beat_ms  = 0;
    bool     beat_on       = true;

    while (true) {
        // 1. USB stack
        tud_task();

        // Heartbeat: toggle GP25 at 2 Hz so we can visually confirm the loop
        // is running even if the WS2812 is not visible.
        {
            uint32_t now_hb = to_ms_since_boot(get_absolute_time());
            if ((now_hb - last_beat_ms) >= 250) {
                last_beat_ms = now_hb;
                beat_on = !beat_on;
                gpio_put(HEARTBEAT_LED_PIN, beat_on);
            }
        }

        // 2. Update LED animation state
        rgb_led_task();

        // 3. Ingest any pending iBUS bytes
        bool new_frame = ibus_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (new_frame) {
            ibus_get_channels(channels, IBUS_NUM_CHANNELS);
            last_frame_ms = now;
            rc_active = true;
        }

        // 4. Failsafe timeout
        if (rc_active && (now - last_frame_ms) > FAILSAFE_TIMEOUT_MS) {
            rc_active = false;
        }

        // 5. Set LED status
        if (!tud_mounted()) {
            rgb_led_set_state(RGB_STATE_BOOT);
        } else if (!rc_active) {
            rgb_led_set_state(ibus_has_lock() ? RGB_STATE_FAILSAFE : RGB_STATE_READY);
        } else if (sticks_centered(channels)) {
            rgb_led_set_state(RGB_STATE_CENTERED);
        } else {
            rgb_led_set_state(RGB_STATE_ACTIVE);
        }

        // 6. Build the HID report
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

        // 7. Send HID report at ~200 Hz max (5 ms). Also rate-limits when
        //    receiver is offline — no point flooding the host with failsafe.
        if ((now - last_send_ms) >= 5) {
            if (send_joystick_report(&report)) {
                last_send_ms = now;
            }
        }
    }
    return 0;
}
