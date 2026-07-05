/*
 * main.c — RC-Joystick firmware entry point.
 *
 * Reads iBUS OR SBUS from a receiver on GP1 (UART0 RX) — same pin, same
 * firmware. At boot the firmware alternately tries iBUS (115200 8N1) and
 * SBUS (100000 8E2, inverted at the GPIO pad) for a few hundred milliseconds
 * each, and locks onto whichever protocol produces a valid frame first.
 * Once locked, that protocol is used for the rest of the session.
 *
 * The 14 (iBUS) or 16 (SBUS) raw channels are mapped onto an 8-axis + 32-button
 * HID report.
 *
 * Failsafe: engaged when any of
 *   - no valid frame for FAILSAFE_TIMEOUT_MS (500 ms), OR
 *   - all channels stay bit-identical for CHANNEL_FREEZE_MS (1000 ms — TX off
 *     with the receiver in "hold" mode; iBUS has no explicit failsafe flag), OR
 *   - (SBUS only) the frame's FAILSAFE or FRAMELOST flag bit is set.
 * On failsafe: all axes center, throttle (Z) to minimum, buttons clear.
 *
 * LED patterns on GP25 (durations sized for easy visual counting):
 *
 *   BOOT           250 on / 250 off                    USB not yet enumerated
 *   WAITING        1000 on / 1000 off                  No protocol locked yet
 *   WRONG_WIRING   3 × (250 on / 250 off) + 1500 off   Bytes arriving, no valid frame
 *   FAILSAFE       4 × (250 on / 250 off) + 1500 off   Signal lost after previously working
 *   ACTIVE         2500 on / 300 off                   Valid frames with moving channels
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_descriptors.h"
#include "ibus.h"
#include "sbus.h"
#include "hardware/uart.h"

#define RC_UART                   uart0
#define RC_RX_PIN                 1u

#define STATUS_LED_PIN            25u
#define FAILSAFE_TIMEOUT_MS       500u
#define WIRE_ACTIVITY_MS          200u
#define CHANNEL_FREEZE_MS         1000u
#define DETECT_TIMEOUT_MS         300u    // per-protocol attempt window

// ─────────────────────────────────────────────────────────────────────────────
// Protocol selection + scaling
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    PROTO_NONE,
    PROTO_IBUS,
    PROTO_SBUS,
} protocol_t;

static protocol_t s_protocol = PROTO_NONE;

// iBUS raw 1000–2000, mid 1500, half-range 500
static inline int16_t scale_ibus(uint16_t raw) {
    int32_t v = (int32_t)raw - 1500;
    if (v >  500) v =  500;
    if (v < -500) v = -500;
    return (int16_t)(v * 32767 / 500);
}

// SBUS raw 172–1811, mid 992, half-range 819
static inline int16_t scale_sbus(uint16_t raw) {
    int32_t v = (int32_t)raw - 992;
    if (v >  819) v =  819;
    if (v < -819) v = -819;
    return (int16_t)(v * 32767 / 819);
}

static inline int16_t scale_axis(uint16_t raw) {
    return (s_protocol == PROTO_SBUS) ? scale_sbus(raw) : scale_ibus(raw);
}

static inline uint16_t protocol_mid(void) {
    return (s_protocol == PROTO_SBUS) ? 992 : 1500;
}

static inline unsigned int protocol_num_channels(void) {
    return (s_protocol == PROTO_SBUS) ? SBUS_NUM_CHANNELS : IBUS_NUM_CHANNELS;
}

static bool active_update(void) {
    switch (s_protocol) {
        case PROTO_IBUS: return ibus_update();
        case PROTO_SBUS: return sbus_update();
        default:         return false;
    }
}
static bool active_get_channels(uint16_t *out, unsigned int n) {
    switch (s_protocol) {
        case PROTO_IBUS: return ibus_get_channels(out, n);
        case PROTO_SBUS: return sbus_get_channels(out, n);
        default:         return false;
    }
}
static bool active_has_lock(void) {
    switch (s_protocol) {
        case PROTO_IBUS: return ibus_has_lock();
        case PROTO_SBUS: return sbus_has_lock();
        default:         return false;
    }
}
static uint32_t active_last_byte_ms(void) {
    switch (s_protocol) {
        case PROTO_IBUS: return ibus_last_byte_ms();
        case PROTO_SBUS: return sbus_last_byte_ms();
        default:         return 0;
    }
}
static bool active_sbus_flag_lost(void) {
    if (s_protocol != PROTO_SBUS) return false;
    uint8_t f = sbus_flags();
    return (f & (SBUS_FLAG_FAILSAFE | SBUS_FLAG_FRAMELOST)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Failsafe report
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
// Boot-time protocol auto-detection
//
// Alternates between iBUS and SBUS setups on the same UART+pin until a valid
// frame is parsed. USB stack is serviced during detection so the host
// doesn't time out enumeration.
// ─────────────────────────────────────────────────────────────────────────────
static bool try_protocol(protocol_t p, uint32_t timeout_ms) {
    switch (p) {
        case PROTO_IBUS:
            ibus_reset_state();
            ibus_init(RC_UART, RC_RX_PIN);
            break;
        case PROTO_SBUS:
            sbus_reset_state();
            sbus_init(RC_UART, RC_RX_PIN, SBUS_PARITY_EVEN);
            break;
        default: return false;
    }

    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
        tud_task();

        // While detecting, show WRONG_WIRING if bytes are arriving but haven't
        // yielded a valid frame yet — makes physical-connection issues obvious.
        led_mode_t detect_mode = LED_MODE_WAITING;
        if (!tud_mounted()) {
            detect_mode = LED_MODE_BOOT;
        } else {
            uint32_t last_byte = (p == PROTO_IBUS)
                                    ? ibus_last_byte_ms()
                                    : sbus_last_byte_ms();
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (last_byte != 0 && (now - last_byte) < WIRE_ACTIVITY_MS) {
                detect_mode = LED_MODE_WRONG_WIRING;
            }
        }
        status_led_update(detect_mode);

        bool locked = (p == PROTO_IBUS) ? ibus_update() : sbus_update();
        if (locked) return true;
    }

    // Timeout — tear down cleanly for the next attempt
    switch (p) {
        case PROTO_IBUS: ibus_deinit(RC_UART, RC_RX_PIN); break;
        case PROTO_SBUS: sbus_deinit(RC_UART, RC_RX_PIN); break;
        default: break;
    }
    return false;
}

static void detect_protocol(void) {
    while (s_protocol == PROTO_NONE) {
        if (try_protocol(PROTO_IBUS, DETECT_TIMEOUT_MS)) { s_protocol = PROTO_IBUS; return; }
        if (try_protocol(PROTO_SBUS, DETECT_TIMEOUT_MS)) { s_protocol = PROTO_SBUS; return; }
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

    tusb_init();
    detect_protocol();

    const unsigned int NCH = protocol_num_channels();
    uint16_t channels[SBUS_NUM_CHANNELS] = {0};        // sized for the larger of the two
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

        bool new_frame = active_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (new_frame) {
            active_get_channels(channels, NCH);
            last_frame_ms = now;

            if (!have_baseline) {
                memcpy(prev_channels, channels, sizeof(uint16_t) * NCH);
                last_change_ms = now;
                have_baseline = true;
            } else {
                bool any_change = false;
                for (unsigned int i = 0; i < NCH; i++) {
                    if (channels[i] != prev_channels[i]) { any_change = true; break; }
                }
                if (any_change) {
                    memcpy(prev_channels, channels, sizeof(uint16_t) * NCH);
                    last_change_ms = now;
                }
            }
            rc_active = true;
        }

        // Failsafe: frame timeout, channel freeze, or SBUS flag
        if (rc_active &&
            ((now - last_frame_ms) > FAILSAFE_TIMEOUT_MS ||
             (have_baseline && (now - last_change_ms) > CHANNEL_FREEZE_MS) ||
             active_sbus_flag_lost())) {
            rc_active = false;
        }

        // ── LED mode ───────────────────────────────────────────────────────
        led_mode_t mode;
        if (!tud_mounted()) {
            mode = LED_MODE_BOOT;
        } else if (rc_active) {
            mode = LED_MODE_ACTIVE;
        } else {
            uint32_t last_byte = active_last_byte_ms();
            bool wire_recent = last_byte != 0 && (now - last_byte) < WIRE_ACTIVITY_MS;
            bool ever_locked = active_has_lock();
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
            uint16_t mid = protocol_mid();
            for (unsigned int i = 8; i < NCH; i++) {
                if (channels[i] > mid) btns |= (1u << (i - 8));
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
