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

/* Take the indicator dark once this board has been quiet for long enough.

   Both modes are a timestamp and a timeout. IDLE measures from the last input this
   board's computer received - the same last_activity the screensaver uses, which the
   inter-board link keeps current for input arriving from the other board too - so the
   light stays on while you work and goes out when you stop. SWITCH measures from the
   moment the output last changed, so it shows the switch and then gets out of the way.

   Only the board that is the active output has anything lit, so this does nothing on
   the other one. The caps lock indicator (kbd_led_as_indicator) is deliberately left
   alone: led_sync_task drives the keyboard's LEDs from keyboard_leds_desired and would
   undo any suppression here within a frame. */
void led_timeout_task(device_t *state) {
    bool suppress = false;

    /* Config mode blinks once a second and that blink is the only sign the device is in
       it, so it outranks the timeout. */
    if (state->config.led_off_mode != LED_ALWAYS_ON && !state->config_mode_active) {
        uint64_t timeout_us = (uint64_t)state->config.led_off_sec * 1000000;
        uint64_t since = state->last_switch_time;

        /* Idle counts from the last input this computer saw, or from the switch if that
           is the more recent of the two: becoming the active output starts the clock as
           much as a keypress does, and without it switching by hotkey onto a computer
           left alone since this morning would light the LED and put it straight back
           out. Counting from the switch alone is the other mode's whole point, so it
           takes the timestamp as it stands. */
        if (state->config.led_off_mode == LED_OFF_WHEN_IDLE
            && state->last_activity[BOARD_ROLE] > since)
            since = state->last_activity[BOARD_ROLE];

        /* Zero seconds means never, so a half-configured device keeps its indicator. */
        suppress = timeout_us && (time_us_64() - since > timeout_us);
    }

    if (suppress == state->led_suppressed)
        return;

    state->led_suppressed = suppress;

    /* Mid-blink the blinking task owns the pin and ends by calling restore_leds()
       itself, which picks this up - stepping in here would cut it short. */
    if (state->blinks_left == 0)
        restore_leds(state);
}
