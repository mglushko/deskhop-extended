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

#include "main.h"

/* ==================================================== *
 * Hotkeys to trigger actions via the keyboard.
 * ==================================================== */

hotkey_combo_t hotkeys[] = {
    /* Main keyboard switching hotkey */
    {.modifier       = HOTKEY_MODIFIER,
     .keys           = {HOTKEY_TOGGLE},
     .key_count      = 1,
     .pass_to_os     = false,
     .action_handler = &output_toggle_hotkey_handler},

    /* Pressing right ALT + right CTRL toggles the slow mouse mode */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL,
     .keys           = {},
     .key_count      = 0,
     .pass_to_os     = true,
     .acknowledge    = true,
     .action_handler = &mouse_zoom_hotkey_handler},

    /* Switch lock */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTCTRL,
     .keys           = {HID_KEY_K},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &switchlock_hotkey_handler},

    /* Screen lock */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTCTRL,
     .keys           = {HID_KEY_L},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &screenlock_hotkey_handler},

    /* Toggle gaming mode */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_G},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &toggle_gaming_mode_handler},

    /* Enable screensaver pong for active output */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_S},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &enable_screensaver_pong_hotkey_handler},

    /* Enable screensaver jitter for active output */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_J},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &enable_screensaver_jitter_hotkey_handler},

    /* Disable screensaver for active output */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_X},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &disable_screensaver_hotkey_handler},

    /* Record switch y coordinate  */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_F12, HID_KEY_Y},
     .key_count      = 2,
     .acknowledge    = true,
     .action_handler = &screen_border_hotkey_handler},

    /* Switch to configuration mode  */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_C, HID_KEY_O},
     .key_count      = 2,
     .acknowledge    = true,
     .action_handler = &config_enable_hotkey_handler},

    /* Hold down left shift + right shift + F12 + A ==> firmware upgrade mode for board A (kbd) */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT | KEYBOARD_MODIFIER_LEFTSHIFT,
     .keys           = {HID_KEY_A},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &fw_upgrade_hotkey_handler_A},

    /* Hold down left shift + right shift + F12 + B ==> firmware upgrade mode for board B (mouse) */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT | KEYBOARD_MODIFIER_LEFTSHIFT,
     .keys           = {HID_KEY_B},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &fw_upgrade_hotkey_handler_B}};

/* config_t stores one packed combo per entry, in this order, so the two have to agree.
   Adding a hotkey means adding an api_field_map entry for it (protocol.c) and a row on
   the config page - and appending it here, never inserting, or every stored combo shifts
   onto the wrong action. */
_Static_assert(ARRAY_SIZE(hotkeys) == NUM_HOTKEYS, "NUM_HOTKEYS no longer matches hotkeys[]");

/* True only while hotkeys[] is being rewritten, on the other core. */
static volatile bool hotkeys_updating = false;

/* Replace the compiled-in combos with whatever is configured.

   hotkeys[] is overwritten in place, so the combos it was compiled with are captured on
   the first call - which is initial_setup, before any config has been applied. That is
   what an entry set back to zero returns to, and what a wiped config restores, without
   needing a reboot. */
/* The keys a packed combo actually names, written in order, and how many there were.
   HID_KEY_NONE pads the empty slots of every keyboard report there is, so a combo that
   asked for it would match everything and take the keyboard with it; the second key
   standing alone becomes the first. */
static uint8_t unpack_keys(uint32_t packed, uint8_t *keys) {
    uint8_t count = 0;

    if (HOTKEY_KEY1(packed) != HID_KEY_NONE)
        keys[count++] = HOTKEY_KEY1(packed);

    if (HOTKEY_KEY2(packed) != HID_KEY_NONE)
        keys[count++] = HOTKEY_KEY2(packed);

    return count;
}

/* A packed combo as an entry, so a stored value and a table row can be compared as one
   thing. Only the three fields a match is decided on are filled in. */
static hotkey_combo_t combo_of(uint32_t packed) {
    hotkey_combo_t combo = {.modifier = HOTKEY_MOD(packed)};

    combo.key_count = unpack_keys(packed, combo.keys);

    return combo;
}

/* Whether an entry already stands for this combination. The same modifiers, and the same
   keys in any order - check_specific_hotkey asks only that each key it names is somewhere
   in the report, so which slot holds which is not part of what a combination is. */
static bool same_combo(const hotkey_combo_t *entry, uint8_t modifier, const uint8_t *keys,
                       uint8_t count) {
    if (entry->modifier != modifier || entry->key_count != count)
        return false;

    for (uint8_t k = 0; k < count; k++) {
        bool found = false;

        for (uint8_t j = 0; j < count; j++)
            found = found || entry->keys[j] == keys[k];

        if (!found)
            return false;
    }

    return true;
}

void hotkeys_apply_config(device_t *state) {
    static uint32_t compiled_in[NUM_HOTKEYS];
    static bool captured = false;
    hotkey_combo_t config_combo = {0};

    /* This runs on core0, from the config endpoint, while core1 is matching keyboard
       reports against the same table. Rather than reason about a half-written entry,
       take the table out of use while it is rewritten - the cost is missing a hotkey
       for the few microseconds it takes. */
    hotkeys_updating = true;

    if (!captured) {
        for (int n = 0; n < NUM_HOTKEYS; n++)
            compiled_in[n] = HOTKEY_PACK(hotkeys[n].modifier, hotkeys[n].keys[0], hotkeys[n].keys[1]);
        captured = true;
    }

    /* Held apart because it is the entry that cannot give way. It is fixed, so where it and
       a stored combo collide the stored one is always the one to go, whichever side of it
       in the table that entry sits. */
    config_combo = combo_of(compiled_in[HOTKEY_CONFIG_IDX]);

    for (int n = 0; n < NUM_HOTKEYS; n++) {
        uint32_t packed = state->config.hotkey_cfg[n];

        /* Both of these clear the stored value rather than stepping around it. Zero already
           means "use the combo this firmware was built with", which is what the entry falls
           back to below, so hotkeys[], what the config API reports back and what the page
           draws all say the same thing - and a config already in flash stops being read as
           an instruction the moment it is loaded, rather than being refused again on every
           boot. They sit ahead of the hotkey_toggle branch deliberately: gating the stored
           value leaves the fallback chain below untouched, where gating the result would
           make a refused entry mean the compiled combo on this call and the hotkey_toggle
           one on the next. */

        /* The config page is the only way to undo a shortcut, and this combo is the only
           way to the config page, so it is not something a shortcut may be set to. */
        if (n == HOTKEY_CONFIG_IDX)
            packed = state->config.hotkey_cfg[n] = 0;

        /* A combo naming no key is matched on its modifiers alone, so it fires on every
           report that merely holds them, and check_all_hotkeys would hand it every report
           an action further down was waiting for. Only an entry compiled without a key of
           its own is meant to work that way. The modifier == 0 half catches a value that is
           non-zero only in the unused top byte, which would otherwise sit in flash forever
           saying nothing. */
        if (packed
            && HOTKEY_KEY1(packed) == HID_KEY_NONE && HOTKEY_KEY2(packed) == HID_KEY_NONE
            && (HOTKEY_MOD(packed) == 0 || HOTKEY_KEY1(compiled_in[n]) != HID_KEY_NONE))
            packed = state->config.hotkey_cfg[n] = 0;

        /* Two entries standing for one combination means the lower of them never fires,
           since check_all_hotkeys hands the report to the first that fits. Refuse the stored
           one rather than let an action go quietly dead. Compared against the entries
           already decided this pass and against config mode; an entry further down that
           collides is caught when its own turn comes, because by then this one is decided. */
        if (packed) {
            hotkey_combo_t want = combo_of(packed);
            bool taken = same_combo(&config_combo, want.modifier, want.keys, want.key_count);

            for (int m = 0; m < NUM_HOTKEYS && !taken; m++) {
                if (m == n)
                    continue;

                /* Entries already decided this pass are read from the table. Ones still to
                   come are only worth comparing where nothing is stored against them, so
                   they are going to land on what they were compiled with and cannot move
                   out of the way; where something is stored, that entry does the comparing
                   when its own turn arrives and this one is decided by then. */
                if (m < n)
                    taken = same_combo(&hotkeys[m], want.modifier, want.keys, want.key_count);
                else if (!state->config.hotkey_cfg[m]) {
                    hotkey_combo_t theirs = combo_of(compiled_in[m]);

                    taken = same_combo(&theirs, want.modifier, want.keys, want.key_count);
                }
            }

            if (taken)
                packed = state->config.hotkey_cfg[n] = 0;
        }

        /* config.hotkey_toggle names the key for the switch combo and predates this
           array. Nothing has ever read it - it is stored, defaulted and writable over the
           config API, while the combo that switches outputs has always been the
           compile-time one. Honour it here, but only where the array says nothing and
           only where it says something other than the key this firmware was built with,
           so a stored config that merely carries the default is not read as an
           instruction to change anything. */
        if (n == 0 && !packed
            && state->config.hotkey_toggle != HOTKEY_TOGGLE
            && state->config.hotkey_toggle != HID_KEY_NONE)
            packed = HOTKEY_PACK(HOTKEY_MODIFIER, state->config.hotkey_toggle, HID_KEY_NONE);

        if (!packed)
            packed = compiled_in[n];

        uint8_t modifier = HOTKEY_MOD(packed);
        uint8_t keys[2]  = {0};
        uint8_t count    = unpack_keys(packed, keys);

        /* A backstop now that the stored value is cleared above: nothing configurable can
           reach here empty, only a compiled-in combo that was itself left empty. Worth the
           two lines, because check_specific_hotkey answers true for every report when both
           of these are zero, which is the worst this table can do. */
        if (count == 0 && modifier == 0)
            continue;

        hotkeys[n].modifier  = modifier;
        hotkeys[n].key_count = count;

        memset(hotkeys[n].keys, 0, sizeof(hotkeys[n].keys));
        memcpy(hotkeys[n].keys, keys, count);
    }

    hotkeys_updating = false;
}

/* ============================================================ *
 * Detect if any hotkeys were pressed
 * ============================================================ */

/* Tries to find if the keyboard report contains key, returns true/false */
bool key_in_report(uint8_t key, const hid_keyboard_report_t *report) {
    for (int j = 0; j < KEYS_IN_USB_REPORT; j++) {
        if (key == report->keycode[j]) {
            return true;
        }
    }

    return false;
}

/* Check if the current report matches a specific hotkey passed on */
bool check_specific_hotkey(hotkey_combo_t keypress, const hid_keyboard_report_t *report) {
    /* We expect all modifiers specified to be detected in the report */
    if (keypress.modifier != (report->modifier & keypress.modifier))
        return false;

    for (int n = 0; n < keypress.key_count; n++) {
        if (!key_in_report(keypress.keys[n], report)) {
            return false;
        }
    }

    /* Getting here means all of the keys were found. */
    return true;
}

/* Go through the list of hotkeys, check if any of them match. */
hotkey_combo_t *check_all_hotkeys(hid_keyboard_report_t *report, device_t *state) {
    hotkey_combo_t *keyless = NULL;

    /* Set while the other core is rewriting the table (hotkeys_apply_config above). */
    if (hotkeys_updating)
        return NULL;

    /* Asked before the loop so that nothing standing above it in the table can answer
       first. Every other entry is settable, and one set to a combination this one contains
       would otherwise take the only way back to the page with it. */
    if (check_specific_hotkey(hotkeys[HOTKEY_CONFIG_IDX], report))
        return &hotkeys[HOTKEY_CONFIG_IDX];

    for (int n = 0; n < ARRAY_SIZE(hotkeys); n++) {
        /* Read once and matched as a copy, so a rewrite landing mid-loop cannot show this
           one entry and then another. */
        hotkey_combo_t combo = hotkeys[n];

        if (!check_specific_hotkey(combo, report))
            continue;

        /* An entry with no key of its own is matched on its modifiers, so it also matches
           every combination built on top of those modifiers. Hold it back and let an entry
           that named a key which was actually pressed answer instead. */
        if (combo.key_count == 0) {
            if (keyless == NULL)
                keyless = &hotkeys[n];

            continue;
        }

        return &hotkeys[n];
    }

    return keyless;
}

/* ==================================================== *
 * Keyboard State Management
 * ==================================================== */

/* Update the keyboard state for a specific device */
void update_kbd_state(device_t *state, hid_keyboard_report_t *report, uint8_t device_idx) {
    /* Ensure device_idx is within bounds */
    if (device_idx >= MAX_DEVICES)
        return;

    /* Update the keyboard state for this device */
    memcpy(&state->local_kbd_states[device_idx], report, sizeof(hid_keyboard_report_t));

    /* Track the largest keyboard index we have */
    if (state->max_kbd_idx < device_idx)
        state->max_kbd_idx = device_idx;
}

/* Update the struct storing the state of the keyboard(s) connected to the other board */
void update_remote_kbd_state(device_t *state, hid_keyboard_report_t *report) {
    memcpy(&state->remote_kbd_state, report, sizeof(hid_keyboard_report_t));
}

/* Add keys from source to destination, avoiding duplicates */
static void add_keys(hid_keyboard_report_t *dest, const hid_keyboard_report_t *src) {
    for (uint8_t i = 0; i < KEYS_IN_USB_REPORT; i++) {
        uint8_t key = src->keycode[i];
        
        if (key == 0 || key_in_report(key, dest))
            continue;
            
        uint8_t *empty_slot = memchr(dest->keycode, 0, KEYS_IN_USB_REPORT);
        if (empty_slot)
            *empty_slot = key;
    }
}

/* Release all keys */
void release_all_keys(device_t *state) {
    memset(state->local_kbd_states, 0, sizeof(state->local_kbd_states));
    memset(&state->remote_kbd_state, 0, sizeof(hid_keyboard_report_t));
    
    static hid_keyboard_report_t empty_report = {0};
    queue_kbd_report(&empty_report, state);
}


/* Combine all keyboard states into a single report */
void combine_kbd_states(device_t *state, hid_keyboard_report_t *combined_report) {
    memset(combined_report, 0, sizeof(hid_keyboard_report_t));

    /* Combine all local keyboards up to max_kbd_idx */
    for (uint8_t i = 0; i <= state->max_kbd_idx; i++) {
        combined_report->modifier |= state->local_kbd_states[i].modifier;
        add_keys(combined_report, &state->local_kbd_states[i]);
    }
    
    /* Add remote keyboard */
    combined_report->modifier |= state->remote_kbd_state.modifier;
    add_keys(combined_report, &state->remote_kbd_state);
}

/* ==================================================== *
 * Keyboard Queue Section
 * ==================================================== */

void process_kbd_queue_task(device_t *state) {
    hid_keyboard_report_t report;

    /* If we're not connected, we have nowhere to send reports to. */
    if (!state->tud_connected)
        return;

    /* Peek first, if there is anything there... */
    if (!queue_try_peek(&state->kbd_queue, &report))
        return;

    /* If we are suspended, let's wake the host up */
    if (tud_suspended())
        tud_remote_wakeup();

    /* If it's not ok to send yet, we'll try on the next pass */
    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    /* ... try sending it to the host, if it's successful.
       In boot protocol (UEFI/BIOS/BitLocker) the host expects the fixed 8-byte
       boot keyboard report with NO report ID, so send the raw report struct
       (modifier, reserved, 6 keycodes). Otherwise use the normal report-ID path. */
    bool succeeded;
    if (tud_hid_n_get_protocol(ITF_NUM_HID) == HID_PROTOCOL_BOOT)
        succeeded = tud_hid_n_report(ITF_NUM_HID, 0, &report, sizeof(report));
    else
        succeeded = tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report.modifier, report.keycode);

    /* ... then we can remove it from the queue. Race conditions shouldn't happen [tm] */
    if (succeeded)
        queue_try_remove(&state->kbd_queue, &report);
}

void queue_kbd_report(hid_keyboard_report_t *report, device_t *state) {
    /* It wouldn't be fun to queue up a bunch of messages and then dump them all on host */
    if (!state->tud_connected)
        return;

    queue_try_add(&state->kbd_queue, report);
}

/* If keys need to go locally, queue packet to kbd queue, else send them through UART */
void send_key(hid_keyboard_report_t *report, device_t *state) {
    /* Create a combined report from all device states */
    hid_keyboard_report_t combined_report;
    combine_kbd_states(state, &combined_report);

    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        /* Queue the combined report */
        queue_kbd_report(&combined_report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        /* Send the combined report to ensure all keys are included */
        queue_packet((uint8_t *)&combined_report, KEYBOARD_REPORT_MSG, KBD_REPORT_LENGTH);
    }
}

/* Decide if consumer control reports go local or to the other board */
void send_consumer_control(uint8_t *raw_report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_cc_packet(raw_report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)raw_report, CONSUMER_CONTROL_MSG, CONSUMER_CONTROL_LENGTH);
    }
}

/* Decide if consumer control reports go local or to the other board */
void send_system_control(uint8_t *raw_report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_system_packet(raw_report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)raw_report, SYSTEM_CONTROL_MSG, SYSTEM_CONTROL_LENGTH);
    }
}

/* ==================================================== *
 * Parse and interpret the keys pressed on the keyboard
 * ==================================================== */

void process_keyboard_report(uint8_t *raw_report, int length, uint8_t itf, hid_interface_t *iface) {
    hid_keyboard_report_t new_report = {0};
    device_t *state                  = &global_state;
    hotkey_combo_t *hotkey           = NULL;

    if (length < KBD_REPORT_LENGTH)
        return;

    /* No more keys accepted if we're about to reboot */
    if (global_state.reboot_requested)
        return;

    extract_kbd_data(raw_report, length, itf, iface, &new_report);

    /* Update the keyboard state for this device */
    update_kbd_state(state, &new_report, itf);

    /* Check if any hotkey was pressed */
    hotkey = check_all_hotkeys(&new_report, state);

    /* ... and take appropriate action */
    if (hotkey != NULL) {
        /* Provide visual feedback we received the action */
        if (hotkey->acknowledge)
            blink_led(state);

        /* Execute the corresponding handler */
        hotkey->action_handler(state, &new_report);

        /* And pass the key to the output PC if configured to do so. */
        if (!hotkey->pass_to_os)
            return;
    }

    /* This method will decide if the key gets queued locally or sent through UART */
    send_key(&new_report, state);
}

void process_consumer_report(uint8_t *raw_report, int length, uint8_t itf, hid_interface_t *iface) {
    uint8_t new_report[CONSUMER_CONTROL_LENGTH] = {0};
    uint16_t *report_ptr = (uint16_t *)new_report;

    device_t *state = &global_state;
    keyboard_t *keyboard = get_keyboard(iface, raw_report[0]);

    /* Only skip the leading byte if this interface actually uses report IDs. Keyboards
       that expose consumer controls on a dedicated interface commonly omit the report
       ID entirely (e.g. Cherry KC 6000), in which case the data starts at byte 0 and
       skipping unconditionally would read the wrong byte - reporting the wrong keys or,
       when the payload is short, none at all. */
    uint8_t *data = raw_report;
    int data_len  = length;

    if (iface->uses_report_id) {
        data++;
        data_len--;
    }

    /* If consumer control is variable, read the values from cc_array and send as array. */
    if (iface->consumer.is_variable) {
        for (int i = 0; i < MAX_CC_BUTTONS && i < 8 * data_len; i++) {
            int bit_idx = i % 8;
            int byte_idx = i >> 3;

            if ((data[byte_idx] >> bit_idx) & 1) {
                report_ptr[0] = keyboard->cc_array[i];
            }
        }
    }
    else {
        for (int i = 0; i < data_len && i < CONSUMER_CONTROL_LENGTH; i++)
            new_report[i] = data[i];
    }

    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        send_consumer_control(new_report, state);
    } else {
        queue_packet((uint8_t *)new_report, CONSUMER_CONTROL_MSG, CONSUMER_CONTROL_LENGTH);
    }
}

void process_system_report(uint8_t *raw_report, int length, uint8_t itf, hid_interface_t *iface) {
    /* As in process_consumer_report, the report ID is only present if the interface
       uses one - without this an interface that omits it either reads the wrong byte
       or gets rejected by the length check below. */
    uint8_t *data = raw_report;
    int data_len  = length;

    if (iface->uses_report_id) {
        data++;
        data_len--;
    }

    if (data_len < SYSTEM_CONTROL_LENGTH)
        return;

    uint16_t new_report = data[0];
    uint8_t *report_ptr = (uint8_t *)&new_report;
    device_t *state = &global_state;

    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        send_system_control(report_ptr, state);
    } else {
        queue_packet(report_ptr, SYSTEM_CONTROL_MSG, SYSTEM_CONTROL_LENGTH);
    }
}

/* Which keyboard on this interface owns this report ID? Lookup only: an ID we have
   never seen falls back to the primary keyboard rather than claiming a slot, because
   this also runs at decode time, where a stray or corrupted leading byte must not
   consume one. Parse time wants the opposite and calls get_or_add_keyboard below.

   The old version short-circuited on `num_keyboards == 1` before it looked at the
   report ID at all. Since num_keyboards reaches 1 on the first collection and the
   allocation path was never reached, it could never exceed 1 on an interface using
   report IDs, so every later collection was handed keyboards[0] and wrote over the
   first one. MAX_KEYBOARDS was unreachable. */
keyboard_t *get_keyboard(hid_interface_t *iface, uint8_t report_id) {
    /* No report IDs on this interface, so there is only ever one keyboard. */
    if (!iface->uses_report_id)
        return &iface->keyboards[PRIMARY_KEYBOARD];

    for (int i = 0; i < iface->num_keyboards && i < MAX_KEYBOARDS; i++) {
        if (iface->keyboards[i].report_id == report_id)
            return &iface->keyboards[i];
    }

    /* If nothing else is matched, return the primary keyboard. */
    return &iface->keyboards[PRIMARY_KEYBOARD];
}

/* Parse-time counterpart: the same lookup, but an unseen report ID claims the next
   free slot. Keyboards are registered here rather than by the caller, so that the
   slot a collection is given while the descriptor is read is the same slot
   get_keyboard() hands back when its reports arrive.

   Runs out of slots by returning the primary keyboard, which is what the code did
   for every collection before, so a keyboard declaring more than MAX_KEYBOARDS
   collections degrades to the old behaviour rather than to something new. */
keyboard_t *get_or_add_keyboard(hid_interface_t *iface, uint8_t report_id) {
    if (!iface->uses_report_id)
        return &iface->keyboards[PRIMARY_KEYBOARD];

    for (int i = 0; i < iface->num_keyboards && i < MAX_KEYBOARDS; i++) {
        if (iface->keyboards[i].report_id == report_id)
            return &iface->keyboards[i];
    }

    if (iface->num_keyboards >= MAX_KEYBOARDS)
        return &iface->keyboards[PRIMARY_KEYBOARD];

    keyboard_t *kb = &iface->keyboards[iface->num_keyboards];

    kb->report_id      = report_id;
    kb->uses_report_id = true;

    return kb;
}
