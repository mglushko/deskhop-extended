/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 *
 * How often the host port polls an interrupt endpoint.
 *
 * A device asks to be polled every bInterval frames and Pico-PIO-USB honours the
 * number verbatim. Some devices ask for far less than they can use. The Logitech
 * Lightspeed receiver in upstream issue #215 is the clear case: it is a high speed
 * device, this port is full speed only, so it falls back to a full speed configuration
 * its vendor says is unsupported and asks to be polled every 10 ms while apparently
 * still producing at its normal rate internally. Its own buffer then stands full and
 * the delay never drains. A Logitech Unifying receiver, which is full speed by design,
 * asks for 8 ms on its keyboard and 2 ms on its mouse. None of them ask for 1.
 *
 * So when the setting is on we stop honouring the number and poll interrupt IN
 * endpoints every frame, which is 1000 Hz, the ceiling this hardware has either way.
 *
 * Two details that are easy to get wrong:
 *
 * bInterval 0 is the slowest case, not the fastest. pio_usb_host.c reloads the
 * countdown with `interval - 1` into a uint8_t, so 0 wraps to 255 and the endpoint is
 * polled once every 256 frames. The test is therefore "not already 1" rather than
 * "greater than 1".
 *
 * bmAttributes has to be masked. Only the low two bits are the transfer type; the rest
 * is synchronisation and usage type, which an interrupt endpoint is allowed to set. This
 * function is handed the raw descriptor byte, so comparing all of it would hand back the
 * declared interval for a perfectly legal interrupt endpoint and leave it slow. (The
 * scheduler in pio_usb_host.c masks correctly; the whole-byte compare there is in
 * enumerate_device, which this build never reaches because TinyUSB drives the host.)
 *
 * Class does not come into it. The endpoint descriptor is all there is at the point this
 * is asked, so a hub's interrupt IN status endpoint is clamped along with everything
 * else. That is deliberate rather than overlooked: it costs one NAKed token per frame,
 * a few microseconds against a thousand, and the alternative is plumbing an interface
 * class down to a layer that has no business knowing one.
 *
 * Low speed devices keep whatever they asked for. USB 2.0 permits 1 to 255 there, so
 * clamping them would be legal, but a low speed transaction costs roughly eight times
 * the wire time and needs a PRE packet ahead of it behind a hub, and no low speed
 * device is implicated in any of this.
 *
 * Kept free of hardware headers so it can be exercised on the host; the endpoint this
 * answers for is opened in Pico-PIO-USB. See misc/hosttest/test_polling.c, run by
 * misc/hosttest/run.sh.
 */

#include "usb_polling.h"

/* bmAttributes bits 1:0, USB 2.0 table 9-13 */
#define EP_XFER_MASK      0x03
#define EP_XFER_INTERRUPT 0x03

/* bEndpointAddress bit 7 */
#define EP_DIR_IN 0x80

#define ONE_FRAME 1

uint8_t usb_polling_interval(uint8_t attr,
                             uint8_t epaddr,
                             uint8_t declared,
                             bool full_speed,
                             bool force_fast) {
    if (!force_fast)
        return declared;

    /* Interrupt IN only. Control, bulk and isochronous are not polled on a schedule we
       set, and an OUT endpoint is written when we have something to write. */
    if ((attr & EP_XFER_MASK) != EP_XFER_INTERRUPT)
        return declared;

    if (!(epaddr & EP_DIR_IN))
        return declared;

    if (!full_speed)
        return declared;

    /* Everything else, 0 included */
    if (declared == ONE_FRAME)
        return declared;

    return ONE_FRAME;
}
