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
#include <math.h>

#define MACOS_SWITCH_MOVE_X 10
#define MACOS_SWITCH_MOVE_COUNT 5
#define WRAP_STEP_ONTO 16
#define WRAP_STEP_ACROSS 1000
#define WRAP_STEPS_PER_SCREEN 16
#define ACCEL_POINTS 7

uint16_t get_jump_threshold(output_t *output, enum screen_pos_e direction) {
    const uint16_t NO_JUMP_THRESHOLD = 0;

    /* With wrap-around, going away from the border on the last screen is a jump to another pc too */
    if (global_state.config.wrap_around && output->pos == direction &&
        output->screen_index >= output->screen_count)
        return global_state.config.jump_threshold;

    /* If on non-main local screen, every possible switch is local */
    if (output->screen_index > 1)
        return NO_JUMP_THRESHOLD;

    /* If on main screen but going away from the border, switch is local */
    if (output->pos == direction && output->screen_index == 1)
        return NO_JUMP_THRESHOLD;

    /* ... in all other cases, switch is non-local (jump to another pc) */
    return global_state.config.jump_threshold;
}

/* Check if our upcoming mouse movement would result in having to switch outputs */
enum screen_pos_e is_screen_switch_needed(output_t *output, int position, int offset) {
    enum screen_pos_e direction = (offset < 0) ? LEFT : RIGHT;

    /* No position offset implies no switch needed. */
    if (offset == 0)
        return NONE;

    /* Local switches (virtual desktop changes) have no gap, only cross-output jumps use threshold */
    uint16_t threshold = get_jump_threshold(output, direction);

    if (position + offset < MIN_SCREEN_COORD - threshold)
        return LEFT;

    if (position + offset > MAX_SCREEN_COORD + threshold)
        return RIGHT;

    return NONE;
}

/* Move mouse coordinate 'position' by 'offset', but don't fall off the screen */
int32_t move_and_keep_on_screen(int position, int offset) {
    /* Lowest we can go is 0 */
    if (position + offset < MIN_SCREEN_COORD)
        return MIN_SCREEN_COORD;

    /* Highest we can go is MAX_SCREEN_COORD */
    else if (position + offset > MAX_SCREEN_COORD)
        return MAX_SCREEN_COORD;

    /* We're still on screen, all good */
    return position + offset;
}

/* Implement basic mouse acceleration based on actual 2D movement magnitude.
   Returns the acceleration factor to apply to both x and y components. */
float calculate_mouse_acceleration_factor(int32_t offset_x, int32_t offset_y) {
    const struct curve {
        int value;
        float factor;
    } acceleration[ACCEL_POINTS] = {
                   // 4 |                                        *
        {2, 1},    //   |                                  *
        {5, 1.1},  // 3 |
        {15, 1.4}, //   |                       *
        {30, 1.9}, // 2 |                *
        {45, 2.6}, //   |        *
        {60, 3.4}, // 1 |  *
        {70, 4.0}, //    -------------------------------------------
    };             //        10    20    30    40    50    60    70

    if (offset_x == 0 && offset_y == 0)
        return 1.0;

    if (!global_state.config.enable_acceleration)
        return 1.0;

    // Calculate the 2D movement magnitude
    const float movement_magnitude = sqrtf((float)(offset_x * offset_x) + (float)(offset_y * offset_y));

    if (movement_magnitude <= acceleration[0].value)
        return acceleration[0].factor;

    if (movement_magnitude >= acceleration[ACCEL_POINTS-1].value)
        return acceleration[ACCEL_POINTS-1].factor;

    const struct curve *lower = NULL;
    const struct curve *upper = NULL;

    for (int i = 0; i < ACCEL_POINTS-1; i++) {
        if (movement_magnitude < acceleration[i + 1].value) {
            lower = &acceleration[i];
            upper = &acceleration[i + 1];
            break;
        }
    }

    // Should never happen, but just in case
    if (lower == NULL || upper == NULL)
        return 1.0;

    const float interpolation_pos = (movement_magnitude - lower->value) /
                                  (upper->value - lower->value);

    return lower->factor + interpolation_pos * (upper->factor - lower->factor);
}

/* Uniform speed: how many pixels a count moves the pointer, slowed the way slow mouse slows
   speed_x and speed_y. */
static float uniform_pixels_per_count(device_t *state, uint8_t reduce_speed) {
    uint16_t percent = state->config.pointer_speed;

    if (percent < POINTER_SPEED_MIN || percent > POINTER_SPEED_MAX)
        percent = POINTER_SPEED;

    return percent / 100.0f / (1 << reduce_speed);
}

/* The resolution of the screen the pointer is on, or the default where nothing usable is
   stored against it. */
static screen_size_t current_screen_size(device_t *state) {
    uint32_t      index = state->config.output[state->active_output].screen_index;
    screen_size_t size  = {SCREEN_WIDTH, SCREEN_HEIGHT};

    if (index >= 1 && index <= MAX_SCREENS_PER_OUTPUT) {
        screen_size_t stored = state->config.screen_size[state->active_output][index - 1];

        if (stored.width >= SCREEN_SIZE_MIN)
            size.width = stored.width;

        if (stored.height >= SCREEN_SIZE_MIN)
            size.height = stored.height;
    }

    return size;
}

/* Uniform speed, one axis: the move in pixels, then in screen coordinates, where crossing the
   screen from one edge to the other is size - 1 pixels.

   On a Windows extra screen Windows moves the cursor itself, a pixel per count at its default
   speed, so it is sent the pixels, whole, in place of the raw move. Where the pointer is then
   follows from what was sent, which puts it where Windows has it and makes the outer edge the
   real one. Everywhere else the board places the pointer itself. Either way the fraction that
   did not make a whole step is carried into the next report, so a slow move is not lost. */
static int uniform_offset(device_t *state, int axis, int32_t *move, float pixels_per_count, uint16_t size) {
    float px = *move * pixels_per_count;

    if (state->relative_mouse && !state->gaming_mode) {
        float want = px + state->uniform_rest_px[axis];
        float sent = roundf(want);

        /* The report carries 16 bits. Past that, drop the rest rather than play it out
           over the reports that follow. */
        if (sent > INT16_MAX)
            sent = INT16_MAX;
        else if (sent < -INT16_MAX)
            sent = -INT16_MAX;

        state->uniform_rest_px[axis] = (fabsf(want - sent) < 1.0f) ? want - sent : 0.0f;
        *move = (int32_t)sent;
        px    = sent;
    }

    float units  = px * MAX_SCREEN_COORD / (size - 1) + state->uniform_rest_units[axis];
    int   offset = (int)roundf(units);

    state->uniform_rest_units[axis] = units - offset;
    return offset;
}

/* Returns LEFT if need to jump left, RIGHT if right, NONE otherwise */
enum screen_pos_e update_mouse_position(device_t *state, mouse_values_t *values) {
    output_t *current    = &state->config.output[state->active_output];
    uint8_t reduce_speed = 0;
    int offset_x, offset_y;

    /* Check if we are configured to move slowly */
    if (state->mouse_zoom)
        reduce_speed = MOUSE_ZOOM_SCALING_FACTOR;

    /* Calculate movement */
    float acceleration_factor = calculate_mouse_acceleration_factor(values->move_x, values->move_y);

    if (state->config.uniform_speed) {
        float         pixels = uniform_pixels_per_count(state, reduce_speed) * acceleration_factor;
        screen_size_t screen = current_screen_size(state);

        offset_x = uniform_offset(state, 0, &values->move_x, pixels, screen.width);
        offset_y = uniform_offset(state, 1, &values->move_y, pixels, screen.height);
    } else {
        offset_x = round(values->move_x * acceleration_factor * (current->speed_x >> reduce_speed));
        offset_y = round(values->move_y * acceleration_factor * (current->speed_y >> reduce_speed));
    }

    /* Determine if our upcoming movement would stay within the screen */
    enum screen_pos_e switch_direction = is_screen_switch_needed(current, state->pointer_x, offset_x);

    /* Update movement */
    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);

    return switch_direction;
}

/* If we are active output, queue packet to mouse queue, else send them through UART */
void output_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_mouse_report(report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)report, MOUSE_REPORT_MSG, MOUSE_REPORT_LENGTH);
    }
}

/* Calculate and return Y coordinate when moving from screen out_from to screen out_to */
int16_t scale_y_coordinate(int screen_from, int screen_to, device_t *state) {
    output_t *from = &state->config.output[screen_from];
    output_t *to   = &state->config.output[screen_to];

    int size_to   = to->border.bottom - to->border.top;
    int size_from = from->border.bottom - from->border.top;

    /* If sizes match, there is nothing to do */
    if (size_from == size_to)
        return state->pointer_y;

    /* Moving from smaller ==> bigger screen
       y_a = top + (((bottom - top) * y_b) / HEIGHT) */

    if (size_from > size_to) {
        return to->border.top + ((size_to * state->pointer_y) / MAX_SCREEN_COORD);
    }

    /* Moving from bigger ==> smaller screen
       y_b = ((y_a - top) * HEIGHT) / (bottom - top) */

    if (state->pointer_y < from->border.top)
        return MIN_SCREEN_COORD;

    if (state->pointer_y > from->border.bottom)
        return MAX_SCREEN_COORD;

    return ((state->pointer_y - from->border.top) * MAX_SCREEN_COORD) / size_from;
}

/* Clear any pending edge double-tap arming. Called whenever the active screen
   changes so a tap registered at one screen's edge can never carry over and
   complete a switch at a different screen or edge. */
static void reset_edge_tap(device_t *state) {
    state->edge_in_contact         = false;
    state->last_edge_tap_time      = 0;
    state->last_edge_tap_direction = NONE;
}

void switch_to_another_pc(
    device_t *state, output_t *output, int output_to, int direction) {
    uint8_t *mouse_park_pos = &state->config.output[state->active_output].mouse_park_pos;

    int16_t mouse_y = (*mouse_park_pos == 0) ? MIN_SCREEN_COORD : /* Top */
                      (*mouse_park_pos == 1) ? MAX_SCREEN_COORD : /* Bottom */
                                               state->pointer_y;  /* Previous */

    mouse_report_t hidden_pointer = {.y = mouse_y, .x = MAX_SCREEN_COORD};

    output_mouse_report(&hidden_pointer, state);
    set_active_output(state, output_to);
    state->pointer_x = (direction == LEFT) ? MAX_SCREEN_COORD : MIN_SCREEN_COORD;
    state->pointer_y = scale_y_coordinate(output->number, 1 - output->number, state);

    /* New screen -> forget any half-completed double-tap. */
    reset_edge_tap(state);

    /* Tell the other board where the cursor actually ended up. There is only one
       cursor but each board tracks it separately, and a pointing device may well be
       attached to the other board (e.g. a keyboard with an integrated trackball).
       This also overwrites the parking coordinates the hidden_pointer report above
       just left there, which would otherwise be picked up as a real cursor position. */
    sync_pointer_position(state);
}

/* Send our current cursor position to the other board so both agree where it is */
void sync_pointer_position(device_t *state) {
    uart_packet_t packet = {
        .type = POINTER_SYNC_MSG,
        .data16 = {
            [0] = (uint16_t)state->pointer_x,
            [1] = (uint16_t)state->pointer_y,
        },
    };

    queue_try_add(&state->uart_tx_queue, &packet);
}

void switch_virtual_desktop_macos(device_t *state, int direction) {
    /*
     * Fix for MACOS: Before sending new absolute report setting X to 0:
     * 1. Move the cursor to the edge of the screen directly in the middle to handle screens
     *    of different heights
     * 2. Send relative mouse movement one or two pixels in the direction of movement to get
     *    the cursor onto the next screen
     */
    mouse_report_t edge_position = {
        .x = (direction == LEFT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD,
        .y = MAX_SCREEN_COORD / 2,
        .mode = ABSOLUTE,
        .buttons = state->mouse_buttons,
    };

    uint16_t move = (direction == LEFT) ? -MACOS_SWITCH_MOVE_X : MACOS_SWITCH_MOVE_X;
    mouse_report_t move_relative_one = {
        .x = move,
        .mode = RELATIVE,
        /* Force buttons to 0 for relative movement to avoid duplicating the button 
           press state, which would leave the relative HID mouse permanently stuck 
           down if the user is dragging an item while switching desktops. */
        .buttons = 0,
    };

    output_mouse_report(&edge_position, state);

    /* Once doesn't seem reliable enough, do it a few times */
    for (int i = 0; i < MACOS_SWITCH_MOVE_COUNT; i++)
        output_mouse_report(&move_relative_one, state);
}

void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    switch (output->os) {
        case MACOS:
            switch_virtual_desktop_macos(state, direction);
            break;

        case WINDOWS:
            /* TODO: Switch to relative-only if index > 1, but keep tabs to switch back */
            state->relative_mouse = (new_index > 1);
            break;

        case LINUX:
        case ANDROID:
        case OTHER:
            /* Linux should treat all desktops as a single virtual screen, so you should leave
            screen_count at 1 and it should just work */
            break;
    }

    state->pointer_x       = (direction == RIGHT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
    output->screen_index = new_index;

    /* New screen -> forget any half-completed double-tap so a tap armed at the
       output border can't complete after crossing between virtual desktops. */
    reset_edge_tap(state);
}

/* Windows maps absolute coordinates onto its main screen only, so its other screens are reached
   with relative movement: put the cursor on the main screen's edge, then walk it out until it
   stops at the far side of the desktop, which is the last screen.

   Windows stops a move that would carry the cursor off every screen at the edge of the screen it
   is on. That is what ends the walk at the far side, but it also means no single move may jump a
   whole screen: one that would end past the far side stops at the edge of the screen it started
   on, so the eight pushes of the full range this used to send never left the main screen. So the
   walk alternates two steps, and neither depends on how wide the screens are. A short one gets
   from an edge onto the next screen, and a long one carries the cursor across the screen it is
   on, or up to that screen's far edge and no further.

   The short step is 16 counts: at least a pixel down to a sixteenth of Windows' default pointer
   speed, and 56 at the fastest with acceleration off. The long step is 1000 counts, about what a
   quick sweep of a real mouse reports at once, so acceleration treats it as it would that; with
   acceleration off it is 1000 pixels at the default speed. Sixteen pairs for each screen past the
   main one cover 16000 pixels at the default speed, or 4000 at a quarter of it, and each report
   takes a millisecond on the wire. */
void push_to_far_side(device_t *state, output_t *output, int direction) {
    int16_t sign = (direction == LEFT) ? -1 : 1;

    mouse_report_t edge   = {.x = state->pointer_x, .y = state->pointer_y, .mode = ABSOLUTE};
    mouse_report_t onto   = {.x = sign * WRAP_STEP_ONTO, .mode = RELATIVE};
    mouse_report_t across = {.x = sign * WRAP_STEP_ACROSS, .mode = RELATIVE};

    output_mouse_report(&edge, state);

    for (uint32_t i = 0; i < WRAP_STEPS_PER_SCREEN * (output->screen_count - 1); i++) {
        output_mouse_report(&onto, state);
        output_mouse_report(&across, state);
    }
}

/* Past the outer edge of the last screen, wrap around to the far side of the other computer. Both
   cursors move against the direction of travel: this one back onto its main screen, where every
   other way out leaves it, and the other one out onto its last screen. */
void wrap_to_another_pc(device_t *state, output_t *output, int direction) {
    output_t *other = &state->config.output[1 - state->active_output];
    int back        = (direction == LEFT) ? RIGHT : LEFT;

    /* A Mac crosses its screens one at a time. Windows gets there with the park report, which is
       absolute and so lands on its main screen. */
    if (output->os == MACOS)
        for (uint32_t i = output->screen_index; i > 1; i--)
            switch_virtual_desktop_macos(state, back);

    output->screen_index = 1;
    switch_to_another_pc(state, output, 1 - state->active_output, direction);

    if (other->os == MACOS)
        for (uint32_t i = other->screen_index; i < other->screen_count; i++)
            switch_virtual_desktop_macos(state, back);

    else if (other->os == WINDOWS && other->screen_count > 1)
        push_to_far_side(state, other, back);

    other->screen_index   = other->screen_count;
    state->relative_mouse = (other->os == WINDOWS && other->screen_count > 1);
}

/* Returns true if an actual-output switch is allowed to proceed right now.

   When the "double tap" feature is enabled, the first time the cursor presses
   against the edge only "arms" the switch - the user has to pull away from the
   edge and press against it again (in the same direction) within the configured
   time window for the switch to actually happen. This only gates switches
   between physical outputs; virtual desktop switching never calls this. */
static bool edge_double_tap_ready(device_t *state, int direction) {
    /* Feature disabled -> always allow immediately (classic behavior). */
    if (!state->config.switch_double_tap_enable)
        return true;

    /* Still pressed against the edge from an earlier report; a fresh tap only
       counts once the cursor has been pulled away from the edge again. */
    if (state->edge_in_contact)
        return false;

    /* We just (re)made contact with the edge - remember it so that continuously
       pushing against the edge is treated as a single tap, not many. */
    state->edge_in_contact = true;

    uint64_t now       = time_us_64();
    uint64_t window_us = (uint64_t)state->config.switch_double_tap_ms * 1000;

    /* Second tap in the same direction within the time window -> switch now. */
    if (state->last_edge_tap_direction == direction && (now - state->last_edge_tap_time) <= window_us) {
        state->last_edge_tap_time      = 0;
        state->last_edge_tap_direction = NONE;
        return true;
    }

    /* Otherwise this is the first tap: record it and wait for the second one. */
    state->last_edge_tap_time      = now;
    state->last_edge_tap_direction = direction;
    return false;
}

/*                               BORDER
                                   |
       .---------.    .---------.  |  .---------.    .---------.    .---------.
      ||    B2   ||  ||    B1   || | ||    A1   ||  ||    A2   ||  ||    A3   ||   (output, index)
      ||  extra  ||  ||   main  || | ||   main  ||  ||  extra  ||  ||  extra  ||   (main or extra)
       '---------'    '---------'  |  '---------'    '---------'    '---------'
          )___(          )___(     |     )___(          )___(          )___(
*/
void do_screen_switch(device_t *state, int direction) {
    output_t *output = &state->config.output[state->active_output];

    /* No switching allowed if explicitly disabled or in gaming mode */
    if (state->switch_lock || state->gaming_mode)
        return;

    /* We want to jump in the direction of the other computer */
    if (output->pos != direction) {
        if (output->screen_index == 1) { /* We are at the border -> switch outputs */
            /* No switching allowed if mouse button is held. Should only apply to the border! */
            if (state->mouse_buttons)
                return;

            /* Optionally require a "double tap" against the edge before switching. */
            if (!edge_double_tap_ready(state, direction))
                return;

            switch_to_another_pc(state, output, 1 - state->active_output, direction);
        }
        /* If here, this output has multiple desktops and we are not on the main one */
        else
            switch_virtual_desktop(state, output, output->screen_index - 1, direction);
    }

    /* We want to jump away from the other computer, only possible if there is another screen to jump to */
    else if (output->screen_index < output->screen_count)
        switch_virtual_desktop(state, output, output->screen_index + 1, direction);

    /* ... or wrap around to the far side of the other computer, unless a mouse button is held */
    else if (state->config.wrap_around && !state->mouse_buttons) {
        /* A jump to the other computer, so the double tap applies here as at the border. */
        if (!edge_double_tap_ready(state, direction))
            return;

        wrap_to_another_pc(state, output, direction);
    }
}

static inline bool extract_value(bool uses_id, int32_t *dst, report_val_t *src, uint8_t *raw_report, int len) {
    /* A report with no payload byte, nothing at all or its report ID alone, carries no field.
       TinyUSB hands on zero-length transfers (a STALL, a zero-length packet, three failed
       transactions), and decoding one as zeros told the host every held button was released. */
    if (len <= uses_id)
        return false;

    /* If HID Report ID is used, the report is prefixed by the report ID so we have to move by 1 byte.
       len has to move with it: descriptor offsets are relative to the payload, so handing
       get_report_value the unshifted length leaves its bound off by one in this frame. */
    if (uses_id) {
        if (*raw_report++ != src->report_id)
            return false;
        len--;
    }

    *dst = get_report_value(raw_report, len, src);
    return true;
}

/* 'state' is unused since the buttons fallback below started reading the interface it was
   handed, but the signature stays: deskhop-hidtests lifts this function verbatim out of this
   file and calls it, so changing the shape would break the harness against every tree at once. */
void extract_report_values(uint8_t *raw_report, int len, device_t *state, mouse_values_t *values, hid_interface_t *iface) {
    /* Interpret values depending on the current protocol used. */
    if (iface->protocol == HID_PROTOCOL_BOOT) {
        hid_mouse_report_t *mouse_report = (hid_mouse_report_t *)raw_report;

        /* hid_mouse_report_t is five bytes, but the boot report is only defined as far as
           buttons/x/y and plenty of mice stop there or after the wheel. Take what arrived
           instead of reading the whole struct out of a shorter buffer. A report too short to
           carry the button byte, the zero-length transfer TinyUSB hands on after a STALL,
           says nothing about the buttons, so they stay as this interface last held them;
           returning zeros for it released them. */
        values->buttons = (len >= 1) ? mouse_report->buttons : iface->mouse_buttons;
        values->move_x  = (len >= 2) ? mouse_report->x : 0;
        values->move_y  = (len >= 3) ? mouse_report->y : 0;
        values->wheel   = (len >= MOUSE_BOOT_REPORT_LEN) ? mouse_report->wheel : 0;
        values->pan     = (len > MOUSE_BOOT_REPORT_LEN) ? mouse_report->pan : 0;
        return;
    }
    mouse_t *mouse = &iface->mouse;
    bool uses_id = iface->uses_report_id;

    extract_value(uses_id, &values->move_x, &mouse->move_x, raw_report, len);
    extract_value(uses_id, &values->move_y, &mouse->move_y, raw_report, len);
    extract_value(uses_id, &values->wheel, &mouse->wheel, raw_report, len);
    extract_value(uses_id, &values->pan, &mouse->pan, raw_report, len);

    /* Buttons live under a different report ID than the axes on some devices (the
       Kensington Expert Mouse puts them on report 1 and X/Y on report 2), so a movement
       report says nothing about them and we keep what this interface last held. Reading
       state->mouse_buttons here would hand back the union across every device and store
       another device's bits in this one's slot, where they would stay held after that
       device let go. */
    if (!extract_value(uses_id, &values->buttons, &mouse->buttons, raw_report, len)) {
        values->buttons = iface->mouse_buttons;
    }
}

mouse_report_t create_mouse_report(device_t *state, mouse_values_t *values) {
    mouse_report_t mouse_report = {
        .buttons = values->buttons,
        .x       = state->pointer_x,
        .y       = state->pointer_y,
        .wheel   = values->wheel,
        .pan     = values->pan,
        .mode    = ABSOLUTE,
    };

    /* Workaround for Windows multiple desktops */
    if (state->relative_mouse || state->gaming_mode) {
        mouse_report.x = values->move_x;
        mouse_report.y = values->move_y;
        mouse_report.mode = RELATIVE;
    }

    return mouse_report;
}

/* What every pointing device on this board is holding down, taken together.

   Each report carries its sender's complete button state, so a trackball reporting
   movement with nothing pressed says "no buttons" just as loudly as a keyboard's mouse
   keys say "left down". Taking the last report at its word lets one device release the
   other's buttons in the middle of a drag (upstream issue #287). The union is the same
   answer combine_kbd_states gives for keyboards.

   Keyed on the interface rather than on the device index process_mouse_report is handed:
   tuh_hid_report_received_cb gives every mouse interface index 1, so two mice would share
   one slot.

   Walked afresh each time rather than kept as a running total, because an interface can
   also stop existing: tuh_hid_umount_cb memsets the whole struct, and a total would still
   be carrying what the departed device held. */
static uint8_t combine_local_mouse_buttons(device_t *state) {
    uint8_t buttons = 0;

    for (int dev = 0; dev < MAX_DEVICES; dev++)
        for (int idx = 0; idx < MAX_INTERFACES; idx++)
            buttons |= state->iface[dev][idx].mouse_buttons;

    return buttons;
}

/* Work out this board's half of the union again and return it, keeping what the output PC
   is told in step. Called on every mouse report, and once a second from the heartbeat so
   that a device which simply stopped existing is noticed even though no report arrived to
   say so. */
uint8_t refresh_local_mouse_buttons(device_t *state) {
    state->local_mouse_buttons = combine_local_mouse_buttons(state);
    state->mouse_buttons       = state->local_mouse_buttons | state->remote_mouse_buttons;

    return state->local_mouse_buttons;
}

/* Take note of the other board's half, keeping what the output PC is told in step. Both
   messages that carry that half come through here - MOUSE_BUTTONS_MSG on every change, and
   the heartbeat once a second as the level that corrects a missed one - so the two cannot
   come to different conclusions.

   Between them, these two are the only writers of state->mouse_buttons, and each restates
   the whole union rather than editing half of it. */
void set_remote_mouse_buttons(device_t *state, uint8_t buttons) {
    state->remote_mouse_buttons = buttons;
    state->mouse_buttons        = state->local_mouse_buttons | state->remote_mouse_buttons;
}

void process_mouse_report(uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface) {
    mouse_values_t values = {0};
    device_t *state = &global_state;

    /* Interpret the mouse HID report, extract and save values we need. */
    extract_report_values(raw_report, len, state, &values, iface);

    /* Narrowed to what the report going out can carry, since mouse_report_t.buttons is a
       single byte. A device that declares more than eight buttons (the Logi Bolt receiver
       declares sixteen) is then compared and stored in the width it will be sent in,
       rather than held at full width here and truncated on the way out. */
    uint8_t buttons = (uint8_t)values.buttons;

    /* If nothing changed, don't send a report. This prevents composite keyboards
       (e.g. QMK) that expose a mouse HID interface from generating spurious
       absolute position reports when they send zero-movement mouse reports during
       keyboard events. Compared against what THIS interface last held: against the
       union, a device holding nothing would stop matching the moment another one
       pressed a button, and every idle report would get through again. */
    if (values.move_x == 0 && values.move_y == 0 &&
        values.wheel == 0 && values.pan == 0 &&
        buttons == iface->mouse_buttons) {
        return;
    }

    /* Remember what this device holds, then send the union of everything held anywhere.
       Done before update_mouse_position so do_screen_switch, further down, still reads a
       current state->mouse_buttons when it decides whether a button is being held. */
    uint8_t previous_local = state->local_mouse_buttons;

    iface->mouse_buttons = buttons;
    refresh_local_mouse_buttons(state);
    values.buttons       = state->mouse_buttons;

    /* The other board needs our half of the union to build the same answer, having no
       other way to hear about a button held on a device attached to us. Only when it
       changes: movement is continuous, buttons are not, and the heartbeat restates it once
       a second anyway. */
    if (state->local_mouse_buttons != previous_local)
        send_value(state->local_mouse_buttons, MOUSE_BUTTONS_MSG);

    /* Calculate and update mouse pointer movement. */
    enum screen_pos_e switch_direction = update_mouse_position(state, &values);

    /* Create the report for the output PC based on the updated values */
    mouse_report_t report = create_mouse_report(state, &values);

    /* Move the mouse, depending where the output is supposed to go */
    output_mouse_report(&report, state);

    /* Release the edge only once the cursor has actually been pulled back onto
       the screen by at least the configured margin. While it stays pinned
       against the edge (even with vertical jitter) it remains "in contact", so
       continuously leaning on the edge is treated as a single tap and won't
       switch on its own. */
    int16_t margin = state->config.switch_double_tap_margin;
    if (state->pointer_x > MIN_SCREEN_COORD + margin &&
        state->pointer_x < MAX_SCREEN_COORD - margin)
        state->edge_in_contact = false;

    /* There is one cursor, but each board tracks its position separately. When we are
       not the active output, output_mouse_report already forwarded the full report and
       the other board adopts our position from it. When we ARE the active output the
       report stays local, so the other board would keep a stale position and any
       pointing device attached to it (e.g. a keyboard with an integrated trackball)
       would make the cursor jump back to wherever that board last thought it was.
       Publish our position so both boards stay in agreement. */
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        sync_pointer_position(state);

    /* We use the mouse to switch outputs, if switch_direction is LEFT or RIGHT */
    if (switch_direction != NONE)
        do_screen_switch(state, switch_direction);
}

/* ==================================================== *
 * Mouse Queue Section
 * ==================================================== */

void process_mouse_queue_task(device_t *state) {
    mouse_report_t report = {0};

    /* We need to be connected to the host to send messages */
    if (!state->tud_connected)
        return;

    /* Peek first, if there is anything there... */
    if (!queue_try_peek(&state->mouse_queue, &report))
        return;

    /* If we are suspended, let's wake the host up */
    if (tud_suspended())
        tud_remote_wakeup();

    /* If it's not ready, we'll try on the next pass */
    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    /* If the interface is configured as a keyboard in boot protocol, discard mouse data. */
    if (report.mode == ABSOLUTE && tud_hid_n_get_protocol(ITF_NUM_HID) == HID_PROTOCOL_BOOT) {
        queue_try_remove(&state->mouse_queue, &report);
        return;
    }

    /* Try sending it to the host, if it's successful */
    bool succeeded
        = tud_mouse_report(report.mode, report.buttons, report.x, report.y, report.wheel, report.pan);

    /* ... then we can remove it from the queue */
    if (succeeded)
        queue_try_remove(&state->mouse_queue, &report);
}

void queue_mouse_report(mouse_report_t *report, device_t *state) {
    /* It wouldn't be fun to queue up a bunch of messages and then dump them all on host */
    if (!state->tud_connected)
        return;

    queue_try_add(&state->mouse_queue, report);
}
