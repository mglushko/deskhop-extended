/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */
#pragma once

#include <stdint.h>
#include "structs.h"
#include "misc.h"
#include "screen.h"

/* Only still used to recognise a config written in the pre key-value layout, where a
   uint32 version sat where CONFIG_STORE_FORMAT sits now. Nothing is discarded for
   failing to match it any more. */
#define CURRENT_CONFIG_VERSION 9

#define CONFIG_MAGIC_HEADER 0xB00B1E5

/* Settings are stored as {key, length, value} triples keyed by api_field_map
   (src/protocol.c), which names a field rather than a position in config_t. Adding,
   removing or reordering struct fields therefore no longer invalidates what is already
   in flash: keys that are gone are skipped, keys that are new keep their default. */
#define CONFIG_STORE_FORMAT 0x4B560001  /* 'KV', format 1 */

/* config.version is writable over the API but must never be restored from storage -
   it describes the layout, not a user setting. */
#define CONFIG_VERSION_KEY  70

typedef struct {
    uint32_t magic_header;
    uint32_t format;
    uint16_t length;   /* bytes of entries following this header */
    uint8_t count;     /* how many entries those bytes hold */
    uint8_t _pad;
} config_store_header_t;

/* The pre key-value layout, frozen.

   config_store_load_legacy() reads a stored page as this and nothing else, so config_t
   is free to grow: a page written by a firmware older than the key-value format is 144
   bytes of exactly these fields in exactly this order, whatever config_t looks like now.
   Do not edit this struct - it describes what is already in other people's flash. */
typedef struct {
    uint32_t magic_header;
    uint32_t version;

    uint8_t force_mouse_boot_mode;
    uint8_t force_kbd_boot_protocol;

    uint8_t kbd_led_as_indicator;
    uint8_t hotkey_toggle;
    uint8_t enable_acceleration;

    uint8_t enforce_ports;
    uint16_t jump_threshold;

    uint8_t switch_double_tap_enable;
    uint16_t switch_double_tap_ms;
    uint16_t switch_double_tap_margin;

    output_t output[NUM_SCREENS];

    uint8_t swap_columns;
    uint8_t _reserved[3];

    uint32_t checksum;
} config_v9_t;

/* output_t sits inside the frozen layout, so a change to it moves bytes that are already
   written. If this fires, config_v9_t needs its own copy of the old output_t. */
_Static_assert(sizeof(config_v9_t) == 144, "the pre key-value config layout has moved");

/*==============================================================================
 *  Configuration Data
 *  Structures and variables related to device configuration.
 *==============================================================================*/

extern const config_t default_config;

/*==============================================================================
 *  Configuration API
 *  Functions and data structures for accessing and modifying configuration.
 *==============================================================================*/

extern const field_map_t api_field_map[];
const field_map_t* get_field_map_entry(uint32_t);
const field_map_t* get_field_map_index(uint32_t);
size_t             get_field_map_length(void);

/*==============================================================================
 *  Configuration Management and Packet Processing
 *  Functions for loading, saving, wiping, and resetting device configuration.
 *==============================================================================*/

void load_config(device_t *);
void queue_cfg_packet(uart_packet_t *, device_t *);
void reset_config_timer(device_t *);
void save_config(device_t *);
bool validate_packet(uart_packet_t *);
void wipe_config(void);
