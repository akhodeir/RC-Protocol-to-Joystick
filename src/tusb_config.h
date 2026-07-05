/*
 * TinyUSB configuration for the RC-Joystick project.
 * Single HID interface, device-only, no CDC/MSC/MIDI.
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

// ── Board / controller ─────────────────────────────────────────────────────
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU            OPT_MCU_RP2040
#endif

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

// ── OS / RTOS backend ──────────────────────────────────────────────────────
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_NONE
#endif

// ── Memory allocator ──────────────────────────────────────────────────────
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

// ── Debug ─────────────────────────────────────────────────────────────────
#define CFG_TUSB_DEBUG          0

// ── Device stack ──────────────────────────────────────────────────────────
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

// Class drivers — only HID
#define CFG_TUD_HID             1
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// HID endpoint buffer size — report is 20 B; round up to 64.
#define CFG_TUD_HID_EP_BUFSIZE  64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
