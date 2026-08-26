/* Host-side tests for the host port's polling interval decision in src/usb_polling.c.
 *
 * A device asks to be polled every bInterval frames and Pico-PIO-USB honours the number
 * verbatim. Some devices ask for far less than they can use, which is what makes a mouse
 * behind a Logitech Lightspeed receiver lag by roughly half a second (hrvach/deskhop#215).
 * These are the rules for when we stop honouring the number.
 *
 * The declared values below are measured rather than invented: 10 is what the #215
 * reporters read off a Lightspeed receiver behind DeskHop, and 8 and 2 are what a
 * Unifying receiver 046d:c52b declares for its keyboard and its mouse.
 *
 * Built and run by run.sh. usb_polling.c links against nothing, so this is the whole
 * program.
 */
#include <stdio.h>

#include "usb_polling.h"

static int failures = 0;

static void check(const char *name, int ok, const char *detail) {
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
           ok ? "" : "  <- ", ok ? "" : (detail ? detail : ""));
    if (!ok)
        failures++;
}

/* bmAttributes / bEndpointAddress values, spelled out so a case reads as a descriptor */
#define INTERRUPT 0x03
#define BULK      0x02
#define ISOC      0x01
#define CONTROL   0x00
#define IN        0x81
#define OUT       0x01

#define FULL_SPEED true
#define LOW_SPEED  false
#define ON         true
#define OFF        false

struct polling_case {
    const char *name;
    uint8_t     attr;
    uint8_t     epaddr;
    uint8_t     declared;
    bool        full_speed;
    bool        force_fast;
    uint8_t     want;
};

static const struct polling_case cases[] = {
    /* The devices this exists for */
    {"lightspeed receiver, 10 ms",      INTERRUPT, IN,  10,  FULL_SPEED, ON,  1},
    {"unifying keyboard, 8 ms",         INTERRUPT, IN,  8,   FULL_SPEED, ON,  1},
    {"unifying mouse, 2 ms",            INTERRUPT, IN,  2,   FULL_SPEED, ON,  1},

    /* bInterval 0 is the slowest case, not the fastest: pio_usb_host.c reloads the
       countdown with interval - 1 into a uint8_t, so 0 wraps to 255 and the endpoint is
       polled once every 256 frames. A rule written as "greater than 1" misses it. */
    {"zero is 256 ms, not fast",        INTERRUPT, IN,  0,   FULL_SPEED, ON,  1},

    /* Already as fast as the bus allows, so nothing to do */
    {"one frame is left alone",         INTERRUPT, IN,  1,   FULL_SPEED, ON,  1},
    {"the slowest legal value",         INTERRUPT, IN,  255, FULL_SPEED, ON,  1},

    /* Only the low two bits of bmAttributes are the transfer type. The rest is
       synchronisation and usage type, which an interrupt endpoint may set, so the test
       has to mask rather than compare the whole byte. */
    {"interrupt with sync bits set",    0x13,      IN,  10,  FULL_SPEED, ON,  1},
    {"interrupt with usage bits set",   0x23,      IN,  10,  FULL_SPEED, ON,  1},
    {"interrupt with both set",         0x33,      IN,  10,  FULL_SPEED, ON,  1},

    /* Everything we do not schedule polls for */
    {"bulk is untouched",               BULK,      IN,  10,  FULL_SPEED, ON,  10},
    {"isochronous is untouched",        ISOC,      IN,  10,  FULL_SPEED, ON,  10},
    {"control is untouched",            CONTROL,   IN,  10,  FULL_SPEED, ON,  10},
    {"interrupt OUT is untouched",      INTERRUPT, OUT, 10,  FULL_SPEED, ON,  10},

    /* Low speed keeps what it asked for: a transaction there costs roughly eight times
       the wire time and needs a PRE packet behind a hub, and no low speed device is
       implicated in any of this. */
    {"low speed at 10 stays 10",        INTERRUPT, IN,  10,  LOW_SPEED,  ON,  10},
    {"low speed at 255 stays 255",      INTERRUPT, IN,  255, LOW_SPEED,  ON,  255},
    {"low speed at 0 stays 0",          INTERRUPT, IN,  0,   LOW_SPEED,  ON,  0},

    /* With the setting off nothing is touched at all, whatever else is true */
    {"off: lightspeed untouched",       INTERRUPT, IN,  10,  FULL_SPEED, OFF, 10},
    {"off: unifying mouse untouched",   INTERRUPT, IN,  2,   FULL_SPEED, OFF, 2},
    {"off: zero untouched",             INTERRUPT, IN,  0,   FULL_SPEED, OFF, 0},
    {"off: low speed untouched",        INTERRUPT, IN,  10,  LOW_SPEED,  OFF, 10},
    {"off: bulk untouched",             BULK,      IN,  10,  FULL_SPEED, OFF, 10},
};

int main(void) {
    char detail[128];

    printf("test_polling\n");

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct polling_case *c = &cases[i];
        uint8_t got = usb_polling_interval(c->attr, c->epaddr, c->declared,
                                           c->full_speed, c->force_fast);

        snprintf(detail, sizeof(detail), "wanted %u, got %u", c->want, got);
        check(c->name, got == c->want, detail);
    }

    /* With the setting off the function is the identity over every input, not just the
       ones spelled out above. */
    int identity = 1;
    for (unsigned attr = 0; attr < 256; attr++)
        for (unsigned declared = 0; declared < 256; declared++)
            if (usb_polling_interval((uint8_t)attr, IN, (uint8_t)declared, true, false)
                != (uint8_t)declared)
                identity = 0;

    check("off is the identity over every attr and interval", identity, NULL);

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
