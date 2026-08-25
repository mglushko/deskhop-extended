#pragma once
/* Stand-in for TinyUSB. Only what the firmware headers and keyboard.c's hotkey half name:
   the report type, the usages the twelve compiled-in combinations are built from, the
   modifier mask bits, and the protocol constant. Values are TinyUSB's, from
   class/hid/hid.h - nothing here checks that, unlike harness.h in deskhop-hidtests, so
   keep the list short and obvious. */

#include <stdbool.h>
#include <stdint.h>

#define TU_ATTR_PACKED __attribute__((packed))

typedef struct TU_ATTR_PACKED {
    uint8_t modifier, reserved, keycode[6];
} hid_keyboard_report_t;

#define HID_KEY_NONE      0x00
#define HID_KEY_A         0x04
#define HID_KEY_B         0x05
#define HID_KEY_C         0x06
#define HID_KEY_G         0x0A
#define HID_KEY_J         0x0D
#define HID_KEY_K         0x0E
#define HID_KEY_L         0x0F
#define HID_KEY_O         0x12
#define HID_KEY_S         0x16
#define HID_KEY_X         0x1B
#define HID_KEY_Y         0x1C
#define HID_KEY_CAPS_LOCK 0x39
#define HID_KEY_F1        0x3A
#define HID_KEY_F12       0x45

#define KEYBOARD_MODIFIER_LEFTCTRL   0x01
#define KEYBOARD_MODIFIER_LEFTSHIFT  0x02
#define KEYBOARD_MODIFIER_LEFTALT    0x04
#define KEYBOARD_MODIFIER_LEFTGUI    0x08
#define KEYBOARD_MODIFIER_RIGHTCTRL  0x10
#define KEYBOARD_MODIFIER_RIGHTSHIFT 0x20
#define KEYBOARD_MODIFIER_RIGHTALT   0x40
#define KEYBOARD_MODIFIER_RIGHTGUI   0x80

#define HID_PROTOCOL_BOOT 0

/* The device-side calls keyboard.c makes once a report is on its way out. Declared here,
   defined as no-ops by whichever test links it. */
bool    tud_suspended(void);
bool    tud_remote_wakeup(void);
bool    tud_hid_n_ready(uint8_t instance);
uint8_t tud_hid_n_get_protocol(uint8_t instance);
bool    tud_hid_n_report(uint8_t instance, uint8_t report_id, const void *report, uint16_t len);
bool    tud_hid_keyboard_report(uint8_t report_id, uint8_t modifier, const uint8_t *keycode);
