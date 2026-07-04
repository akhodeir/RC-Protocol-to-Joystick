/*
 * usb_descriptors.c — TinyUSB device, config, string, and HID report descriptors.
 * Also implements the mandatory callbacks and a small send helper.
 */
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

// ─────────────────────────────────────────────────────────────────────────────
// Device Descriptor
// VID 0x2E8A = Raspberry Pi; PID 0x000A = generic HID (development).
// ─────────────────────────────────────────────────────────────────────────────
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,
    .idProduct          = 0x000A,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&s_device_desc;
}

// ─────────────────────────────────────────────────────────────────────────────
// HID Report Descriptor
//
// 8 × 16-bit signed axes (X/Y/Z/Rx/Ry/Rz/Slider/Dial) followed by 32 buttons.
// Total report size: 8*2 + 4 = 20 bytes.
//
// HID_LOGICAL_MIN_N / MAX_N with n=2 emits the two-byte form required for
// -32768..32767. The plain HID_LOGICAL_MIN macro is single-byte only.
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t s_hid_report_desc[] = {
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP        ),
    HID_USAGE      ( HID_USAGE_DESKTOP_JOYSTICK    ),
    HID_COLLECTION ( HID_COLLECTION_APPLICATION    ),

        // 8 axes
        HID_USAGE_PAGE   ( HID_USAGE_PAGE_DESKTOP        ),
        HID_USAGE        ( HID_USAGE_DESKTOP_X           ),
        HID_USAGE        ( HID_USAGE_DESKTOP_Y           ),
        HID_USAGE        ( HID_USAGE_DESKTOP_Z           ),
        HID_USAGE        ( HID_USAGE_DESKTOP_RX          ),
        HID_USAGE        ( HID_USAGE_DESKTOP_RY          ),
        HID_USAGE        ( HID_USAGE_DESKTOP_RZ          ),
        HID_USAGE        ( HID_USAGE_DESKTOP_SLIDER      ),
        HID_USAGE        ( HID_USAGE_DESKTOP_DIAL        ),
        HID_LOGICAL_MIN_N( -32768, 2                     ),
        HID_LOGICAL_MAX_N(  32767, 2                     ),
        HID_REPORT_COUNT ( 8                             ),
        HID_REPORT_SIZE  ( 16                            ),
        HID_INPUT        ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),

        // 32 buttons
        HID_USAGE_PAGE   ( HID_USAGE_PAGE_BUTTON         ),
        HID_USAGE_MIN    ( 1                             ),
        HID_USAGE_MAX    ( 32                            ),
        HID_LOGICAL_MIN  ( 0                             ),
        HID_LOGICAL_MAX  ( 1                             ),
        HID_REPORT_COUNT ( 32                            ),
        HID_REPORT_SIZE  ( 1                             ),
        HID_INPUT        ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),

    HID_COLLECTION_END
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return s_hid_report_desc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration Descriptor (Config + Interface + HID + Endpoint)
// ─────────────────────────────────────────────────────────────────────────────
#define CONFIG_TOTAL_LEN   (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_HID          0x81   // EP1 IN

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,                                  // bConfigurationValue
        1,                                  // bNumInterfaces
        0,                                  // iConfiguration
        CONFIG_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
        100                                 // 100 * 2 mA = 200 mA
    ),
    TUD_HID_DESCRIPTOR(
        0,                                  // interface number
        0,                                  // iInterface
        HID_ITF_PROTOCOL_NONE,              // not a boot-protocol device
        sizeof(s_hid_report_desc),
        EPNUM_HID,
        CFG_TUD_HID_EP_BUFSIZE,
        5                                   // bInterval — 5 ms poll
    ),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_config_desc;
}

// ─────────────────────────────────────────────────────────────────────────────
// String descriptors
// ─────────────────────────────────────────────────────────────────────────────
static const char *s_strings[] = {
    (const char[]){0x09, 0x04},   // 0: langid (en-US) — special-cased below
    "rc-joystick",                // 1: Manufacturer
    "RC-Joystick",                // 2: Product
    "000001",                     // 3: Serial
};

static uint16_t s_string_buf[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        s_string_buf[1] = 0x0409;      // English (US)
        chr_count = 1;
    } else {
        if (index >= sizeof(s_strings) / sizeof(s_strings[0])) return NULL;
        const char *str = s_strings[index];
        size_t len = strlen(str);
        if (len > 31) len = 31;
        for (size_t i = 0; i < len; i++) {
            s_string_buf[1 + i] = (uint16_t)str[i];  // ASCII → UTF-16 LE
        }
        chr_count = (uint8_t)len;
    }
    s_string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_string_buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// HID class callbacks (host GET/SET_REPORT). We don't act on these.
// ─────────────────────────────────────────────────────────────────────────────
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    memset(buffer, 0, reqlen);
    return reqlen;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)bufsize;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public helper
// ─────────────────────────────────────────────────────────────────────────────
bool send_joystick_report(const joystick_report_t *report) {
    if (!tud_hid_ready()) return false;
    return tud_hid_report(0, (void const *)report, sizeof(joystick_report_t));
}
