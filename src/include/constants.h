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
#include <stddef.h>
#include <stdint.h>

/*==============================================================================
 *  Board and Output Roles
 *==============================================================================*/

#define OUTPUT_A 0
#define OUTPUT_B 1

/*==============================================================================
 *  HID Interface Numbers
 *==============================================================================*/

#define ITF_NUM_HID        0
#define ITF_NUM_HID_REL_M  1
#define ITF_NUM_HID_VENDOR 2
#define ITF_NUM_MSC        3

/*==============================================================================
 *  Mouse Modes
 *==============================================================================*/

#define ABSOLUTE 0
#define RELATIVE 1
#define TOUCH 2

/* Which HID interface a mouse report of this mode goes out on. Absolute reports share
   interface 0 with the keyboard, consumer and system report IDs; relative reports have
   interface 1 to themselves. Both tud_mouse_report() and process_mouse_queue_task()'s
   readiness check ask here, so the endpoint that is waited on is always the endpoint
   that is then written. */
static inline uint8_t mouse_report_instance(uint8_t mode) {
    return (mode == RELATIVE) ? ITF_NUM_HID_REL_M : ITF_NUM_HID;
}

/*==============================================================================
 *  Boolean States
 *==============================================================================*/

#define ENABLE  1
#define DISABLE 0

/*==============================================================================
 *  Numerical Constants
 *==============================================================================*/

#define CONFIG_MODE_TIMEOUT 300000000 // 5 minutes into the future
#define JITTER_DISTANCE 2
#define MOUSE_BOOT_REPORT_LEN 4
#define MOUSE_ZOOM_SCALING_FACTOR 2
#define NUM_SCREENS 2

/* Entries in hotkeys[] (keyboard.c). Pinned here because config_t stores one packed
   combo per entry; keyboard.c asserts the table still matches. */
#define NUM_HOTKEYS 12

/* Where the config-mode combo sits in hotkeys[]. It is the only way into the config page,
   and the page is the only way to undo a shortcut, so unlike every other entry this one is
   fixed: hotkeys_apply_config drops whatever is stored against it, and check_all_hotkeys
   tries it before the rest so nothing above it can answer first. Move the entry and this
   moves with it. */
#define HOTKEY_CONFIG_IDX 9

/*==============================================================================
 *  Utility Macros
 *==============================================================================*/

#define _HZ(x) ((uint64_t)((1000000) / (x)))
#define _MS(x) (x * 1000)
#define _SEC(x) (x * 1000000)
#define _TOP()  0
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define CURRENT_BOARD_IS_ACTIVE_OUTPUT (global_state.active_output == global_state.board_role)

uint32_t crc32_iter(uint32_t crc, const uint8_t byte);
uint32_t calc_crc32(const uint8_t *s, size_t n);
