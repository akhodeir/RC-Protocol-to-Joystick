/*
 * main.c — RC-Joystick firmware entry point.
 *
 * Reads SBUS from a receiver on GP1 (UART0 RX). Currently hardcoded to SBUS
 * for debugging; auto-detection (iBUS/SBUS) will be re-added once SBUS is
 * proven working on the target hardware. See git history for the autodetect
 * scaffolding — it's temporarily bypassed, not deleted.
 *
 * SBUS: 100000 baud, 8E2 (even parity, 2 stop bits), INVERTED signal.
 * Polarity flipped at the GPIO pad via gpio_set_inover; PL011 sees a
 * standard-polarity UART frame.
 *
 * Maps 16 raw SBUS channels onto an 8-axis + 32-button HID report.
 *
 * Failsafe: engaged when
 *   - no valid frame for FAILSAFE_TIMEOUT_MS, OR
 *   - all channels stay bit-identical for CHANNEL_FREEZE_MS (TX off / hold), OR
 *   - the SBUS FAILSAFE or FRAMELOST flag bit is set in the frame.
 * On failsafe: all axes center, throttle (Z) to minimum, buttons clear.
 *
 * LED patterns on GP25 (all pulse durations sized for easy visual counting):
 *
 *   BOOT           250 on / 250 off                USB not yet enumerated
 *   WAITING        1000 on / 1000 off              USB up, no bytes on the wire
 *   WRONG_WIRING   3 × (250 on / 250 off) + 1500 off  Bytes arriving, no valid frame
 *   FAILSAFE       4 × (250 on / 250 off) + 1500 off  Was working, then lost signal
 *   ACTIVE         2500 on / 300 off               Valid frames with moving channels
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_descriptors.h"
#include "sbus.h"
#include "hardware/uart.h"

#define RC_UART                   uart0
#define RC_RX_PIN                 1u

#define STATUS_LED_PIN            25u
#define FAILSAFE_TIMEOUT_MS       500u
#define WIRE_ACTIVITY_MS          200u
#define CHANNEL_FREEZE_MS         1000u

// ─────────────────────────────────────────────────────────────────────────────
// Channel → axis scaling (int16 signed, -32767..32767)
// SBUS raw 172–1811, midpoint 992
// ─────────────────────────────────────────────────────────────────────────────
static inline int16_t scale_axis(uint16_t raw) {
    int32_t v = (int32_t)raw - 992;
    if (v >  819) v =  819;
    if (v < -819) v = -819;
    return (int16_t)(v * 32767 / 819);
}

// ─────────────────────────────────────────────────────────────────────────────
// Failsafe report — all centered, throttle to min, buttons clear.
// ─────────────────────────────────────────────────────────────────────────────
static void set_failsafe_report(joystick_report_t *r) {
    memset(r, 0, sizeof(*r));
    r->z = -32767;
}

// ─────────────────────────────────────────────────────────────────────────────
// LED pattern engine — durations tuned for slow, eye-friendly counting
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    LED_MODE_BOOT,
    LED_MODE_WAITING,
    LED_MODE_WRONG_WIRING,
    LED_MODE_FAILSAFE,
    LED_MODE_ACTIVE,
} led_mode_t;

static const uint16_t s_pattern_boot[]         = { 250, 250, 0 };
static const uint16_t s_pattern_waiting[]      = { 1000, 1000, 0 };
static const uint16_t s_pattern_wrong_wiring[] = { 250, 250, 250, 250, 250, 1500, 0 };
static const uint16_t s_pattern_failsafe[]     = { 250, 250, 250, 250, 250, 250, 250, 1500, 0 };
static const uint16_t s_pattern_active[]       = { 2500, 300, 0 };

static const uint16_t *pattern_for(led_mode_t m) {
    switch (m) {
        case LED_MODE_BOOT:         return s_pattern_boot;
        case LED_MODE_WAITING:      return s_pattern_waiting;
        case LED_MODE_WRONG_WIRING: return s_pattern_wrong_wiring;
        case LED_MODE_FAILSAFE:     return s_pattern_failsafe;
        case LED_MODE_ACTIVE:       return s_pattern_active;
    }
    return s_pattern_boot;
}

static void status_led_update(led_mode_t mode) {
    static led_mode_t last_mode = LED_MODE_BOOT;
    static uint8_t    idx = 0;
    static uint32_t   step_start_ms = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (mode != last_mode) {
        last_mode = mode;
        idx = 0;
        step_start_ms = now;
        gpio_put(STATUS_LED_PIN, 1);
        return;
    }

    const uint16_t *pat = pattern_for(mode);
    uint16_t dur = pat[idx];
    if (dur == 0) {
        idx = 0;
        gpio_put(STATUS_LED_PIN, 1);
        step_start_ms = now;
        return;
    }
    if ((now - step_start_ms) >= dur) {
        idx++;
        if (pat[idx] == 0) idx = 0;
        gpio_put(STATUS_LED_PIN, (idx % 2) == 0);
        step_start_ms = now;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main — hardcoded SBUS
// ─────────────────────────────────────────────────────────────────────────────
int main(void) {
    stdio_init_all();

    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_PIN, 0);

    tusb_init();

    // Bring up SBUS on the shared RC pin. Standard 8E2 first; if you need
    // to try the 8N2 variant, change the last argument to SBUS_PARITY_NONE.
    sbus_init(RC_UART, RC_RX_PIN, SBUS_PARITY_EVEN);

    uint16_t channels[SBUS_NUM_CHANNELS] = {0};
    uint16_t prev_channels[SBUS_NUM_CHANNELS] = {0};

    joystick_report_t report;
    set_failsafe_report(&report);

    bool     rc_active      = false;
    uint32_t last_frame_ms  = 0;
    uint32_t last_change_ms = 0;
    bool     have_baseline  = false;
    uint32_t last_send_ms   = 0;

    while (true) {
        tud_task();

        bool new_frame = sbus_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (new_frame) {
            sbus_get_channels(channels, SBUS_NUM_CHANNELS);
            last_frame_ms = now;

            if (!have_baseline) {
                memcpy(prev_channels, channels, sizeof(prev_channels));
                last_change_ms = now;
                have_baseline = true;
            } else {
                bool any_change = false;
                for (int i = 0; i < SBUS_NUM_CHANNELS; i++) {
                    if (channels[i] != prev_channels[i]) { any_change = true; break; }
                }
                if (any_change) {
                    memcpy(prev_channels, channels, sizeof(prev_channels));
                    last_change_ms = now;
                }
            }
            rc_active = true;
        }

        // Failsafe: frame timeout, channel freeze, or SBUS flag
        uint8_t flags = sbus_flags();
        bool sbus_lost_flag = (flags & (SBUS_FLAG_FAILSAFE | SBUS_FLAG_FRAMELOST)) != 0;
        if (rc_active &&
            ((now - last_frame_ms) > FAILSAFE_TIMEOUT_MS ||
             (have_baseline && (now - last_change_ms) > CHANNEL_FREEZE_MS) ||
             sbus_lost_flag)) {
            rc_active = false;
        }

        // ── LED mode ───────────────────────────────────────────────────────
        led_mode_t mode;
        if (!tud_mounted()) {
            mode = LED_MODE_BOOT;
        } else if (rc_active) {
            mode = LED_MODE_ACTIVE;
        } else {
            uint32_t last_byte = sbus_last_byte_ms();
            bool wire_recent = last_byte != 0 && (now - last_byte) < WIRE_ACTIVITY_MS;
            bool ever_locked = sbus_has_lock();
            if (ever_locked) {
                mode = LED_MODE_FAILSAFE;
            } else if (wire_recent) {
                mode = LED_MODE_WRONG_WIRING;
            } else {
                mode = LED_MODE_WAITING;
            }
        }
        status_led_update(mode);

        // ── Build HID report ───────────────────────────────────────────────
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
            for (int i = 8; i < SBUS_NUM_CHANNELS; i++) {
                if (channels[i] > 992) btns |= (1u << (i - 8));
            }
            report.buttons = btns;
        }

        // ── Send HID at ~200 Hz max ────────────────────────────────────────
        if ((now - last_send_ms) >= 5) {
            if (send_joystick_report(&report)) last_send_ms = now;
        }
    }
    return 0;
}
