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

_Static_assert(MAX_DEVICES <= CFG_TUH_DEVICE_MAX,
               "MAX_DEVICES must not exceed CFG_TUH_DEVICE_MAX");

/* ================================================== *
 * ===========  TinyUSB Device Callbacks  =========== *
 * ================================================== */

/* Invoked when we get GET_REPORT control request.
 * We are expected to fill buffer with the report content, update reqlen
 * and return its length. We return 0 to STALL the request.
 *
 * Returning 0 for everything answered badly in two different ways. With a report
 * ID the host gets a one-byte reply carrying nothing but the ID echoed back,
 * since TinyUSB counts that byte before calling us. Without one, which is how a
 * boot-protocol host asks, the count stays at zero and the request stalls. Both
 * are wrong for a report we put in our own descriptor, and the HID spec makes
 * Get_Report mandatory. It went unnoticed while nothing bound this interface as
 * a keyboard, which a boot keyboard now does.
 *
 * Only ITF_NUM_HID has anything to answer with, being the interface that
 * declares a keyboard input report and an LED output report. A boot-protocol
 * host asks with no report ID at all and a report-protocol one asks by ID, so
 * take both. TinyUSB writes the ID byte itself and shortens request_len to
 * match whenever the ID is non-zero, so what belongs in the buffer here is the
 * payload alone either way.
 *
 * Everything else still returns 0. That stalls, which is the correct answer for
 * a report this device does not declare. */
uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t request_len) {
    if (instance != ITF_NUM_HID)
        return 0;

    if (report_id != 0 && report_id != REPORT_ID_KEYBOARD)
        return 0;

    if (report_type == HID_REPORT_TYPE_OUTPUT) {
        if (request_len < 1)
            return 0;

        /* What the host last asked for, which is also what the attached keyboard
           was told, since kbd_led_as_indicator rewrites the bit before it is
           stored rather than on the way out. */
        buffer[0] = global_state.keyboard_leds_desired[BOARD_ROLE];

        return 1;
    }

    if (report_type == HID_REPORT_TYPE_INPUT) {
        hid_keyboard_report_t report = {0};

        if (request_len < sizeof(report))
            return 0;

        /* Only the computer being typed into is told what is held down. Key state is tracked
           on whichever board the keyboard is plugged into, whatever output is selected, so
           answering this from that state alone would let an idle computer read back, over its
           own control pipe, what is being typed into the other one. It is told what it is
           actually receiving, which is nothing. */
        if (CURRENT_BOARD_IS_ACTIVE_OUTPUT)
            combine_kbd_states(&global_state, &report);

        memcpy(buffer, &report, sizeof(report));

        return sizeof(report);
    }

    return 0;
}

/**
 * Computer controls our LEDs by sending USB SetReport messages with a payload
 * of just 1 byte and report type output. It's type 0x21 (USB_REQ_DIR_OUT |
 * USB_REQ_TYP_CLASS | USB_REQ_REC_IFACE) Request code for SetReport is 0x09,
 * report type is 0x02 (HID_REPORT_TYPE_OUTPUT). We get a set_report callback
 * from TinyUSB device HID and then figure out what to do with the LEDs.
 */
void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize) {

    /* We received a report on the config report ID */
    if (instance == ITF_NUM_HID_VENDOR && report_id == REPORT_ID_VENDOR) {
        /* Security - only if config mode is enabled are we allowed to do anything. While the report_id
           isn't even advertised when not in config mode, security must always be explicit and never assume */
        if (!global_state.config_mode_active)
            return;

        /* We insist on a fixed size packet. No overflows. */
        if (bufsize != RAW_PACKET_LENGTH)
            return;

        uart_packet_t *packet = (uart_packet_t *) (buffer + START_LENGTH);

        /* Only a certain packet types are accepted */
        if (!validate_packet(packet))
            return;

        process_packet(packet, &global_state);
    }

    /* Only other set report we care about is LED state change, and that's exactly 1 byte long.
       It belongs to the keyboard interface, the only one of ours that declares an output report
       at all. In boot protocol the host sends that report with no report ID in front of it, so
       accept report ID 0 there too while boot protocol is the one in force. */
    bool is_led_report = instance == ITF_NUM_HID
                      && (report_id == REPORT_ID_KEYBOARD
                          || (report_id == 0
                              && tud_hid_n_get_protocol(ITF_NUM_HID) == HID_PROTOCOL_BOOT));

    if (!is_led_report || bufsize != 1 || report_type != HID_REPORT_TYPE_OUTPUT)
        return;

    uint8_t leds = buffer[0];

    /* If we are using caps lock LED to indicate the chosen output, that has priority */
    if (global_state.config.kbd_led_as_indicator) {
        leds = leds & 0xFD; /* 1111 1101 (Clear Caps Lock bit) */

        if (global_state.active_output)
            leds |= KEYBOARD_LED_CAPSLOCK;
    }

    global_state.keyboard_leds_desired[BOARD_ROLE] = leds;

    /* If the board has a keyboard connected directly, restore those leds. */
    if (global_state.keyboard_connected && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        restore_leds(&global_state);

    /* Always send to the other one, so it is aware of the change */
    send_value(leds, KBD_SET_REPORT_MSG);
}

/* Invoked when device is mounted */
void tud_mount_cb(void) {
    global_state.tud_connected = true;
}

/* Invoked when device is unmounted */
void tud_umount_cb(void) {
    global_state.tud_connected = false;
}

#ifdef DH_DEBUG_CDC_FLASH
void tud_cdc_rx_cb(uint8_t itf) {
    char buf[64];
    uint32_t count = tud_cdc_n_available(itf);

    if (count == 0)
        return;

    if (count > sizeof(buf))
        count = sizeof(buf);

    tud_cdc_n_read(itf, buf, count);

    if (count >= 5 && memcmp(buf, "flash", 5) == 0) {
        reset_usb_boot(0, 0);
    }
}
#endif

/* ================================================== *
 * ===============  USB HOST Section  =============== *
 * ================================================== */

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            global_state.keyboard_connected = false;
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            global_state.mouse_connected = false;
            break;
    }

    /* Also clear the interface structure, otherwise plugging something else later
       might be a fun (and confusing) experience */
    memset(iface, 0, sizeof(hid_interface_t));
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    /* Get interface information */
    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    iface->protocol = tuh_hid_get_protocol(dev_addr, instance);

    /* Parse the report descriptor into our internal structure. */
    parse_report_descriptor(iface, desc_report, desc_len);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (global_state.config.enforce_ports && BOARD_ROLE == OUTPUT_B)
                return;

            if (global_state.config.force_kbd_boot_protocol)
                tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

            /* Keeping this is required for setting leds from device set_report callback */
            global_state.kbd_dev_addr       = dev_addr;
            global_state.kbd_instance       = instance;
            global_state.keyboard_connected = true;
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            if (global_state.config.enforce_ports && BOARD_ROLE == OUTPUT_A)
                return;

            if (global_state.config.force_mouse_boot_mode) {
                /* User requested boot mode - simpler protocol for compatibility.
                   Note: many mice still send wheel data even in boot mode. */
                tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
            } else {
                /* Switch to using report protocol instead of boot, it's more complicated but
                   at least we get all the information we need (looking at you, mouse wheel) */
                if (tuh_hid_get_protocol(dev_addr, instance) == HID_PROTOCOL_BOOT) {
                    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);
                }
            }
            global_state.mouse_connected = true;
            break;

        case HID_ITF_PROTOCOL_NONE:
            break;
    }

    /* Also set mouse_connected if report descriptor contains mouse, even if interface
       protocol says keyboard. This handles composite devices like QMK. */
    if (iface->mouse.is_found) {
        global_state.mouse_connected = true;
    }

    /* Flash local led to indicate a device was connected */
    blink_led(&global_state);

    /* Also signal the other board to flash LED, to enable easy verification if serial works */
    send_value(ENABLE, FLASH_LED_MSG);

    /* Kick off the report querying */
    tuh_hid_receive_report(dev_addr, instance);
}

/* Does the report on the wire carry a leading report ID?
 *
 * uses_report_id is a fact about the *descriptor*, decided once at enumeration.
 * Boot protocol overrides it: the device then sends the fixed boot layout, which
 * carries no report ID whatever the descriptor declared. Reading report[0] as an ID
 * in that mode indexes report_handler[] with the modifier byte on a keyboard, or the
 * button byte on a mouse, so the report lands on whichever receiver that number
 * happens to select - usually none at all, and on a receiver that declares consumer
 * or system collections on low report IDs, the wrong one.
 *
 * The decode side already draws this distinction: extract_kbd_data and
 * extract_report_values both branch on HID_PROTOCOL_BOOT before they ever consult
 * uses_report_id. This is the same rule applied one level earlier, at routing. */
static inline bool report_carries_id(const hid_interface_t *iface) {
    return iface->uses_report_id && iface->protocol != HID_PROTOCOL_BOOT;
}

/* Which receiver does this report belong to? A pure function of the interface and
   the bytes that arrived, deliberately kept separate from the callback below so it
   can be tested off the device - deskhop-hidtests lifts it verbatim rather than
   maintaining its own copy of these rules, which is how the boot-protocol case
   above went unnoticed for as long as it did. Returns NULL when the report has
   nowhere to go. */
process_report_f pick_receiver(const hid_interface_t *iface, uint8_t itf_protocol,
                               uint8_t const *report) {
    if (report_carries_id(iface) || itf_protocol == HID_ITF_PROTOCOL_NONE) {
        uint8_t report_id = 0;

        if (report_carries_id(iface))
            report_id = report[0];

        if (report_id >= MAX_REPORTS)
            return NULL;

        return iface->report_handler[report_id];
    }

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
        return process_keyboard_report;

    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
        return process_mouse_report;

    return NULL;
}

/* Invoked when received report from device via interrupt endpoint */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    /* Calculate a device index that distinguishes between different devices
       while staying within the bounds of MAX_DEVICES.

       Device index assignment:
       - 0: Primary keyboard (the one set in tuh_hid_mount_cb)
       - 1: Mouse devices
       - MAX_DEVICES-2: Secondary keyboards (e.g., wireless keyboard through unified dongle)
       - (dev_addr-1) % (MAX_DEVICES-1): Other devices

       Note: Slot MAX_DEVICES-1 is reserved for the remote device (used in handle_keyboard_uart_msg) */
    uint8_t device_idx;

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        if (dev_addr == global_state.kbd_dev_addr && instance == global_state.kbd_instance) {
            /* Primary keyboard */
            device_idx = 0;
        } else {
            /* Secondary keyboard (e.g., wireless keyboard through unified dongle) */
            device_idx = (MAX_DEVICES - 2);
        }
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        /* Mouse devices */
        device_idx = 1;
    } else {
        /* Other devices */
        device_idx = (dev_addr - 1) % (MAX_DEVICES - 1);
    }

    process_report_f receiver = pick_receiver(iface, itf_protocol, report);

    if (receiver != NULL)
        receiver((uint8_t *)report, len, device_idx, iface);

    /* Continue requesting reports */
    tuh_hid_receive_report(dev_addr, instance);
}

/* Set protocol in a callback. This is tied to an interface, not a specific report ID */
void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol) {
    if (dev_addr > MAX_DEVICES || idx >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][idx];
    iface->protocol = protocol;
}
