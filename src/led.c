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
 * ========== Update pico and keyboard LEDs  ========== *
 * ==================================================== */

void set_keyboard_leds(uint8_t requested_led_state, device_t *state) {
    static uint8_t new_led_value;

    new_led_value = requested_led_state;
    if (state->keyboard_connected) {
        if(tuh_hid_set_report(state->kbd_dev_addr,
                              state->kbd_instance,
                              0,
                              HID_REPORT_TYPE_OUTPUT,
                              &new_led_value,
                              sizeof(uint8_t)))

            state->keyboard_leds_actual[BOARD_ROLE] = requested_led_state;
    }
}

void restore_leds(device_t *state) {
    /* Light up on-board LED if current board is active output, unless the status LED
       timeout has taken it dark (led_timeout_task below). */
    state->onboard_led_state = (state->active_output == BOARD_ROLE) && !state->led_suppressed;
    gpio_put(GPIO_LED_PIN, state->onboard_led_state);

    /* Light up appropriate keyboard leds (if it's connected locally) */
    if (state->keyboard_connected) {
        uint8_t leds = state->keyboard_leds_desired[state->active_output];
        set_keyboard_leds(leds, state);
    }
}

uint8_t toggle_led(void) {
    uint8_t new_led_state = gpio_get(GPIO_LED_PIN) ^ 1;
    gpio_put(GPIO_LED_PIN, new_led_state);

    return new_led_state;
}

void blink_led(device_t *state) {
    /* Since LEDs might be ON previously, we go OFF, ON, OFF, ON, OFF */
    state->blinks_left     = 5;
    state->last_led_change = time_us_32();
}

void led_sync_task(device_t *state) {
    /* Check if keyboard LEDs need to be updated */
    if (state->keyboard_connected) {
        uint8_t desired_leds = state->keyboard_leds_desired[state->active_output];

        if (state->keyboard_leds_actual[BOARD_ROLE] != desired_leds)
            set_keyboard_leds(desired_leds, state);
    }
}

void led_blinking_task(device_t *state) {
    const int blink_interval_us = 80000; /* 80 ms off, 80 ms on */
    static uint8_t leds;

    /* If there is no more blinking to be done, exit immediately */
    if (state->blinks_left == 0)
        return;

    /* We have some blinks left to do, check if they are due, exit if not */
    if ((time_us_32()) - state->last_led_change < blink_interval_us)
        return;

    /* Toggle the LED state */
    uint8_t new_led_state = toggle_led();

    /* Also keyboard leds (if it's connected locally) since on-board leds are not visible */
    leds = new_led_state * 0x07; /* Numlock, capslock, scrollock */

    if (state->keyboard_connected)
        set_keyboard_leds(leds, state);

    /* Decrement the counter and update the last-changed timestamp */
    state->blinks_left--;
    state->last_led_change = time_us_32();

    /* Restore LEDs in the last pass */
    if (state->blinks_left == 0)
        restore_leds(state);
}

/* Take the indicator dark once this board has nothing left to say with it.

   Each mode is one or two rules, and a rule is a timestamp with a timeout: "no input for
   this long" measures from last_activity[BOARD_ROLE] - the same timestamp the keep-awake
   modes use, which the inter-board link keeps current for input arriving from the other
   board - and "this long since the switch" measures from last_switch_time. Any rule that
   is still running keeps the light on, so the two can be combined without one cutting the
   other short.

   Only the board that is the active output has anything lit, so this does nothing on the
   other one. The caps lock indicator (kbd_led_as_indicator) is deliberately left alone:
   led_sync_task drives the keyboard's LEDs from keyboard_leds_desired and would undo any
   suppression here within a frame. */
static bool led_rule_running(uint64_t now, uint64_t since, uint16_t seconds) {
    /* Zero seconds means never, so a rule left unset keeps the indicator rather than
       putting it out on a half-configured device. */
    return seconds == 0 || (now - since) <= (uint64_t)seconds * 1000000;
}

void led_timeout_task(device_t *state) {
    const config_t *config = &state->config;
    uint64_t now = time_us_64();
    uint64_t idle_since = state->last_activity[BOARD_ROLE];
    uint64_t switch_since = state->last_switch_time;
    bool lit = true;

    switch (config->led_off_mode) {
        case LED_OFF_WHEN_IDLE:
            /* Becoming the active output starts the clock as much as a keypress does.
               Without it, switching by hotkey onto a computer left alone since the
               morning would light the LED and put it straight back out. */
            lit = led_rule_running(now, idle_since, config->led_off_sec)
               || led_rule_running(now, switch_since, config->led_off_sec);
            break;

        case LED_OFF_AFTER_SWITCH:
            lit = led_rule_running(now, switch_since, config->led_switch_sec);
            break;

        case LED_OFF_IDLE_AND_SWITCH:
            /* Each on its own timer, so a short one on the switch can mark the change
               without shortening the one on your typing, and the other way round. */
            lit = led_rule_running(now, idle_since, config->led_off_sec)
               || led_rule_running(now, switch_since, config->led_switch_sec);
            break;

        default:
            /* LED_ALWAYS_ON, and anything a newer config page might have stored. */
            break;
    }

    /* Config mode blinks once a second and that blink is the only sign the device is in
       it, so it outranks every rule above. */
    bool suppress = !lit && !state->config_mode_active;

    if (suppress == state->led_suppressed)
        return;

    state->led_suppressed = suppress;

    /* Mid-blink the blinking task owns the pin and ends by calling restore_leds()
       itself, which picks this up - stepping in here would cut it short. */
    if (state->blinks_left == 0)
        restore_leds(state);
}
