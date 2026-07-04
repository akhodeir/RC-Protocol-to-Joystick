/*
 * main.c — RC-Joystick firmware entry point.
 *
 * Reads FlySky iBUS on UART0 (GP1), maps 14 channels onto an 8-axis + 32-button
 * HID report, and blinks the on-board LED on GP25 to indicate status.
 *
 * Failsafe: if no valid frame is received for FAILSAFE_TIMEOUT_MS, all axes
 * center, throttle (Z) forces to minimum, and buttons clear.
 *
 * LED patterns on GP25 (each pattern is a repeating sequence of ON/OFF durations):
 *
 *   BOOT           fast even blink 100/100      USB not yet enumerated
 *   WAITING        slow even blink 500/500      USB up, no bytes ever on iBUS wire
 *   WRONG_WIRING   3 rapid pulses + pause       Bytes arriving but no valid iBUS frames
 *   FAILSAFE       2 rapid pulses + pause       Was receiving, then lost signal (>500 ms)
 *   ACTIVE         healthy heartbeat 1900/100   Valid iBUS frames arriving normally
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

#define STATUS_LED_PIN            25u    // YD-RP2040 on-board LED
#define FAILSAFE_TIMEOUT_MS       500u   // valid-frame timeout before entering failsafe
#define WIRE_ACTIVITY_MS          200u   // any raw byte within this window = "wire active"

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
// Failsafe report — all centered, throttle to min, buttons clear.
// ─────────────────────────────────────────────────────────────────────────────
static void set_failsafe_report(joystick_report_t *r) {
    memset(r, 0, sizeof(*r));
    r->z = -32767;
}

// ─────────────────────────────────────────────────────────────────────────────
// LED pattern engine
//
// Each mode is a small ON/OFF sequence in milliseconds. The driver walks
// through the sequence and loops. The sequence terminates with a 0-length
// entry to mark the loop point.
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    LED_MODE_BOOT,           // USB not enumerated
    LED_MODE_WAITING,        // USB up, no bytes on iBUS wire yet
    LED_MODE_WRONG_WIRING,   // Bytes arriving but no valid frames — bad wiring / protocol
    LED_MODE_FAILSAFE,       // Was receiving valid frames, now signal lost
    LED_MODE_ACTIVE,         // Valid frames arriving
} led_mode_t;

// Each pattern is {on, off, on, off, ..., 0} — the 0 marks end / restart.
static const uint16_t s_pattern_boot[]         = { 100, 100, 0 };
static const uint16_t s_pattern_waiting[]      = { 500, 500, 0 };
static const uint16_t s_pattern_wrong_wiring[] = { 80, 120, 80, 120, 80, 700, 0 };
static const uint16_t s_pattern_failsafe[]     = { 100, 150, 100, 700, 0 };
static const uint16_t s_pattern_active[]       = { 1900, 100, 0 };

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
    static bool       on = false;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Mode change → restart the sequence from the beginning
    if (mode != last_mode) {
        last_mode = mode;
        idx = 0;
        step_start_ms = now;
        on = true;
        gpio_put(STATUS_LED_PIN, 1);
        return;
    }

    const uint16_t *pat = pattern_for(mode);
    uint16_t dur = pat[idx];
    if (dur == 0) {  // end-of-pattern → restart
        idx = 0;
        dur = pat[0];
        on = true;
        gpio_put(STATUS_LED_PIN, 1);
        step_start_ms = now;
        return;
    }

    if ((now - step_start_ms) >= dur) {
        idx++;
        if (pat[idx] == 0) idx = 0;  // wrap
        on = !on;                     // even indexes are ON, odd are OFF (starting with ON)
        gpio_put(STATUS_LED_PIN, (idx % 2) == 0);
        step_start_ms = now;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(void) {
    stdio_init_all();

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
        tud_task();

        bool new_frame = ibus_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (new_frame) {
            ibus_get_channels(channels, IBUS_NUM_CHANNELS);
            last_frame_ms = now;
            rc_active = true;
        }
        if (rc_active && (now - last_frame_ms) > FAILSAFE_TIMEOUT_MS) {
            rc_active = false;
        }

        // ── Decide LED mode ────────────────────────────────────────────────
        led_mode_t mode;
        if (!tud_mounted()) {
            mode = LED_MODE_BOOT;
        } else if (rc_active) {
            mode = LED_MODE_ACTIVE;
        } else {
            uint32_t last_byte = ibus_last_byte_ms();
            bool wire_recent = last_byte != 0 && (now - last_byte) < WIRE_ACTIVITY_MS;
            bool ever_locked = ibus_has_lock();
            if (ever_locked) {
                mode = LED_MODE_FAILSAFE;      // was working, now signal lost
            } else if (wire_recent) {
                mode = LED_MODE_WRONG_WIRING;  // bytes but never a valid frame
            } else {
                mode = LED_MODE_WAITING;       // nothing on wire
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
            for (int i = 8; i < IBUS_NUM_CHANNELS; i++) {
                if (channels[i] > CH_MID) btns |= (1u << (i - 8));
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
