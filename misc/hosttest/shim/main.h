/* Host stand-in for src/include/main.h.
 *
 * config_store.c is deliberately free of hardware access so its format can be exercised
 * off-device. It still includes main.h like every other source file, so this shim sits
 * earlier on the include path and supplies only what the project's real headers need
 * from the Pico SDK, TinyUSB and hid_parser.h. structs.h, config.h, protocol.h and
 * constants.h below are the real ones.
 *
 * hid_interface_t is a stub rather than the real type. It appears in device_t only
 * *after* the config field that every api_field_map offset points into, so the offsets
 * under test are the same ones the firmware computes. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TU_ATTR_PACKED __attribute__((packed))

/* pico/util/queue.h */
typedef struct { void *data; uint16_t wptr, rptr; } queue_t;

/* tusb.h */
typedef struct TU_ATTR_PACKED {
    uint8_t modifier, reserved, keycode[6];
} hid_keyboard_report_t;

/* hid_parser.h */
#define MAX_DEVICES    4
#define MAX_INTERFACES 12
typedef struct { uint8_t _stub; } hid_interface_t;

#include "constants.h"
#include "structs.h"
#include "config.h"
#include "config_store.h"
#include "protocol.h"

/* protocol.c also holds packet-queueing helpers that reach into USB descriptors and
   the SDK's queue. This test only uses the field map from that file, so these exist
   purely so the translation unit compiles and links. */
enum { REPORT_ID_VENDOR = 1, REPORT_ID_CONSUMER = 2, REPORT_ID_SYSTEM = 3 };

static inline bool queue_try_add(queue_t *queue, const void *value) {
    (void)queue; (void)value; return true;
}

static inline void write_raw_packet(uint8_t *dst, uart_packet_t *packet) {
    (void)dst; (void)packet;
}
