/*
 * usb_descriptors.h — HID joystick device: 8 axes × 16-bit + 32 buttons.
 *
 * Axis mapping (RC channel → HID usage):
 *   CH1 → X       (Roll / Aileron)
 *   CH2 → Y       (Pitch / Elevator)
 *   CH3 → Z       (Throttle)
 *   CH4 → Rx      (Yaw / Rudder)
 *   CH5 → Ry
 *   CH6 → Rz
 *   CH7 → Slider
 *   CH8 → Dial
 *   CH9..CH14 → buttons 0..5 (threshold at protocol midpoint)
 */
#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"

// 20 bytes total (8×2 + 4). Packed to match the HID report descriptor exactly.
typedef struct TU_ATTR_PACKED {
    int16_t  x;         // CH1 Roll
    int16_t  y;         // CH2 Pitch
    int16_t  z;         // CH3 Throttle
    int16_t  rx;        // CH4 Yaw
    int16_t  ry;        // CH5
    int16_t  rz;        // CH6
    int16_t  slider;    // CH7
    int16_t  dial;      // CH8
    uint32_t buttons;   // 32 buttons; bit 0 = CH9 > mid, etc.
} joystick_report_t;

// Attempt to send a HID report. Returns false if USB not ready.
bool send_joystick_report(const joystick_report_t *report);

#endif /* USB_DESCRIPTORS_H */
