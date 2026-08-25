/* Host stand-in for src/include/main.h, used by test_config_store only.
 *
 * config_store.c is deliberately free of hardware access so its format can be exercised
 * off-device. It still includes main.h like every other source file, so this shim sits
 * earlier on the include path and supplies only what the project's real headers need
 * from the Pico SDK and TinyUSB - shim/sdk holds those. constants.h, structs.h, config.h
 * and protocol.h below are the real ones.
 *
 * hid_interface_t is a stub rather than the real type, so this stays free of hid_parser.h
 * and the SDK behind it. It appears in device_t only *after* the config field that every
 * api_field_map offset points into, so the offsets under test are the same ones the
 * firmware computes. test_hotkeys needs the real thing and does not use this file at all:
 * it builds against the real main.h, over stub SDK headers. See run.sh. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pico/util/queue.h>
#include <tusb.h>

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
   the SDK's queue. This test only uses the field map from that file, so this exists purely
   so the translation unit compiles; test_config_store.c defines the rest. */
enum { REPORT_ID_VENDOR = 1, REPORT_ID_CONSUMER = 2, REPORT_ID_SYSTEM = 3 };

void write_raw_packet(uint8_t *dst, uart_packet_t *packet);
