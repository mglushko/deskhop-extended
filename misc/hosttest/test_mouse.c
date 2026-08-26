/* Host-side tests for combining mouse buttons in src/mouse.c.
 *
 * A mouse report carries the full button state of the device that sent it, so a trackball
 * reporting movement with nothing pressed says "no buttons" just as loudly as a keyboard's
 * mouse keys say "left down". Whoever reported last used to win, which cancelled the other
 * device's button mid-drag (hrvach/deskhop#287). What goes to the host is now the union
 * across every interface on both boards, and these are the rules that union follows.
 *
 * Everything is driven in boot protocol: that decode path is plain struct reads, so a
 * device can be stood up here without a report descriptor and the cases stay about
 * combining rather than about parsing. src/mousetest.c in deskhop-hidtests covers the
 * descriptor side against real devices.
 *
 * Built and run by run.sh. Everything below the tests is a stand-in for the parts of the
 * firmware mouse.c links against but none of this exercises.
 */
#include <stdio.h>
#include <string.h>

#include "main.h"

static int failures = 0;

static void check(const char *name, int ok, const char *detail) {
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
           ok ? "" : "  <- ", ok ? "" : (detail ? detail : ""));
    if (!ok)
        failures++;
}

/* ==================================================== *
 * What left the board
 * ==================================================== */

static mouse_report_t queued;        /* Last report handed to the local mouse queue */
static bool           queued_any;
static mouse_report_t forwarded;     /* Last report sent to the other board over UART */
static bool           forwarded_any;
static uint8_t        sent_buttons;  /* Last MOUSE_BUTTONS_MSG payload */
static int            sent_count;    /* How many of those went out */

static void forget_output(void) {
    queued_any = forwarded_any = false;
    sent_count = 0;
    memset(&queued, 0, sizeof(queued));
    memset(&forwarded, 0, sizeof(forwarded));
}

/* ==================================================== *
 * Standing up devices and feeding them reports
 * ==================================================== */

static hid_interface_t *plug_in(uint8_t dev_addr, uint8_t instance) {
    hid_interface_t *iface = &global_state.iface[dev_addr - 1][instance];

    memset(iface, 0, sizeof(*iface));
    iface->protocol = HID_PROTOCOL_BOOT;

    return iface;
}

/* A device that declares its buttons wider than the byte the outgoing report carries: the
   Logi Bolt receiver declares sixteen of them, and extract_report_values hands back the
   whole signed field. Standing that up needs the report-protocol path, so the descriptor
   values come from the stubbed get_report_value below rather than from a real descriptor. */
static int32_t wide_buttons;

static hid_interface_t *plug_in_wide(uint8_t dev_addr, uint8_t instance) {
    hid_interface_t *iface = &global_state.iface[dev_addr - 1][instance];

    memset(iface, 0, sizeof(*iface));
    iface->protocol       = HID_PROTOCOL_REPORT;
    iface->uses_report_id = false;
    iface->mouse.is_found = true;

    return iface;
}

/* What tuh_hid_umount_cb does when a device goes away. */
static void unplug(hid_interface_t *iface) {
    memset(iface, 0, sizeof(*iface));
}

/* One boot-protocol report: buttons, then relative X and Y. Held in a buffer longer than
   the length passed in, and the bytes past that length are filled rather than left zero:
   a read past the end then shows up as a wrong value instead of as the zero the guarded
   code writes anyway, and it is not an ASan report about this test's own stack either. */
static void feed(hid_interface_t *iface, uint8_t buttons, int8_t dx, int8_t dy) {
    uint8_t report[8];

    memset(report, 0x7F, sizeof(report));
    report[0] = buttons;
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;

    forget_output();
    process_mouse_report(report, 3, 1, iface);
}

/* The report-protocol path reads its fields through get_report_value, so the bytes handed
   over here say nothing. What the device is reporting is in wide_buttons. */
static void feed_report(hid_interface_t *iface) {
    uint8_t report[8] = {0};

    forget_output();
    process_mouse_report(report, sizeof(report), 1, iface);
}

/* Both boards agree the cursor is in the middle of the screen, so nothing that follows is
   anywhere near an edge and no case accidentally switches outputs. */
static void reset_state(void) {
    memset(&global_state, 0, sizeof(global_state));

    global_state.tud_connected = true;
    global_state.active_output = OUTPUT_A;
    global_state.board_role    = OUTPUT_A;
    global_state.pointer_x     = MAX_SCREEN_COORD / 2;
    global_state.pointer_y     = MAX_SCREEN_COORD / 2;

    for (int out = 0; out < NUM_SCREENS; out++) {
        global_state.config.output[out].number        = out;
        global_state.config.output[out].screen_count  = 1;
        global_state.config.output[out].screen_index  = 1;
        global_state.config.output[out].speed_x       = 16;
        global_state.config.output[out].speed_y       = 16;
        global_state.config.output[out].border.bottom = MAX_SCREEN_COORD;
    }

    /* As defaults.c has them: the other computer is off to the left of A. */
    global_state.config.output[OUTPUT_A].pos = RIGHT;
    global_state.config.output[OUTPUT_B].pos = LEFT;

    forget_output();
}

static const char *seen(void) {
    static char buf[96];

    snprintf(buf, sizeof(buf), "queued=%s%02x forwarded=%s%02x state=%02x sent=%d/%02x",
             queued_any ? "" : "(none) ", queued.buttons,
             forwarded_any ? "" : "(none) ", forwarded.buttons,
             (unsigned)global_state.mouse_buttons, sent_count, sent_buttons);

    return buf;
}

/* Defined with the other stand-ins below main. */
extern mouse_report_t staged;
extern bool           staged_any;
extern uint8_t        asked_instance;
extern uint8_t        wrote_instance;

/* Hand one report to the drain task and report which interface it waited on. */
static uint8_t drain(uint8_t mode) {
    staged = (mouse_report_t){.mode = mode};
    staged_any = true;
    asked_instance = wrote_instance = 0xFF;

    process_mouse_queue_task(&global_state);

    staged_any = false;
    return asked_instance;
}

int main(void) {
    hid_interface_t *keys, *ball, *far, *wide;

    printf("\n  two devices, one board\n\n");

    /* This is #287: mouse keys on a keyboard hold the button, a trackball does the moving. */
    reset_state();
    keys = plug_in(1, 0);
    ball = plug_in(2, 0);

    feed(keys, 0x01, 0, 0);
    check("a button held on one device reaches the host", queued_any && queued.buttons == 0x01, seen());

    feed(ball, 0x00, 5, 0);
    check("the other device moving does not release it", queued_any && queued.buttons == 0x01, seen());
    check("and the move is still a move", queued_any && queued.x > MAX_SCREEN_COORD / 2, seen());

    feed(keys, 0x00, 0, 0);
    check("releasing on the device that pressed lets go", queued_any && queued.buttons == 0x00, seen());

    /* Two devices holding different buttons, each let go separately. */
    reset_state();
    keys = plug_in(1, 0);
    ball = plug_in(2, 0);

    feed(keys, 0x01, 0, 0);
    feed(ball, 0x02, 1, 0);
    check("two devices holding different buttons hold both",
          queued_any && queued.buttons == 0x03, seen());

    feed(keys, 0x00, 0, 0);
    check("one letting go leaves the other's button held",
          queued_any && queued.buttons == 0x02, seen());

    /* The far corner of the interface table, to show the walk covers all of it. */
    reset_state();
    keys = plug_in(1, 0);
    far  = plug_in(MAX_DEVICES, MAX_INTERFACES - 1);

    feed(far, 0x04, 0, 0);
    feed(keys, 0x00, 3, 0);
    check("the last slot in the table counts too", queued_any && queued.buttons == 0x04, seen());

    /* Unplugging is the only thing that clears an interface, and it has to drop its bits. */
    reset_state();
    keys = plug_in(1, 0);
    ball = plug_in(2, 0);

    feed(keys, 0x01, 0, 0);
    unplug(keys);
    feed(ball, 0x00, 2, 0);
    check("unplugging a device drops what it was holding",
          queued_any && queued.buttons == 0x00, seen());

    printf("\n  reports that say nothing new\n\n");

    reset_state();
    keys = plug_in(1, 0);
    ball = plug_in(2, 0);

    feed(ball, 0x00, 0, 0);
    check("an idle report from an idle device is dropped", !queued_any, seen());

    feed(keys, 0x01, 0, 0);
    feed(ball, 0x00, 0, 0);
    check("still dropped while another device holds a button", !queued_any, seen());

    feed(keys, 0x01, 0, 0);
    check("and a device repeating its own held button is dropped too", !queued_any, seen());

    printf("\n  more buttons than the report can carry\n\n");

    /* Sixteen declared buttons, only eight of which fit in mouse_report_t.buttons. The
       stored state has to be narrowed to that same byte, or a device holding one of the
       top eight never matches what it last held and every idle report gets through. */
    reset_state();
    wide = plug_in_wide(1, 0);

    wide_buttons = 0x8001;   /* buttons 1 and 16 */
    feed_report(wide);
    check("the low eight are what reaches the host", queued_any && queued.buttons == 0x01, seen());

    feed_report(wide);
    check("and repeating it says nothing new", !queued_any, seen());

    wide_buttons = (int32_t)(int16_t)0x8000;   /* button 16 alone, sign extended */
    feed_report(wide);
    check("dropping to a button that does not fit lets go",
          queued_any && queued.buttons == 0x00, seen());

    feed_report(wide);
    check("and holding it says nothing new either", !queued_any, seen());

    printf("\n  the other board\n\n");

    /* A pointing device can be attached to either board and neither board sees the other's
       reports, so each announces its own half of the union with MOUSE_BUTTONS_MSG whenever
       it changes. Arrivals come in through set_remote_mouse_buttons, which is what both
       handlers call, so the cases below drive the real thing rather than poking the field.
       handlers.c itself is not linked here: it reaches for the bootrom, the watchdog and
       PICO_DEFAULT_LED_PIN, none of which this shim carries. */
    reset_state();
    keys = plug_in(1, 0);

    /* The other board already holds a button of its own, so a report that announced the
       union instead of our half of it would be caught by what follows. */
    set_remote_mouse_buttons(&global_state, 0x02);

    feed(keys, 0x01, 0, 0);
    check("a local press is announced to the other board", sent_count == 1 && sent_buttons == 0x01,
          seen());
    check("what the announcement carries is our half, not the union",
          sent_buttons == 0x01 && queued_any && queued.buttons == 0x03, seen());

    feed(keys, 0x01, 4, 0);
    check("moving with it still held announces nothing further", sent_count == 0, seen());

    feed(keys, 0x00, 0, 0);
    check("releasing is announced, again as our half", sent_count == 1 && sent_buttons == 0x00,
          seen());
    check("and the other board's button is still held", queued_any && queued.buttons == 0x02,
          seen());

    /* Arriving values, and what they do to what the host is told. */
    reset_state();
    keys = plug_in(1, 0);

    feed(keys, 0x01, 0, 0);
    set_remote_mouse_buttons(&global_state, 0x02);
    check("taking the other board's half in recomputes what the host is told",
          global_state.mouse_buttons == 0x03, seen());

    set_remote_mouse_buttons(&global_state, 0x00);
    check("and dropping it leaves only ours", global_state.mouse_buttons == 0x01, seen());

    /* And in the other direction. */
    reset_state();
    ball = plug_in(2, 0);
    set_remote_mouse_buttons(&global_state, 0x01);

    feed(ball, 0x00, 6, 0);
    check("a local move carries the other board's button",
          queued_any && queued.buttons == 0x01, seen());
    check("and does not announce it back as ours", sent_count == 0, seen());

    /* The heartbeat carries the same value as a level once a second, which is what heals a
       phantom left by a dropped announcement or by a board that restarted holding a
       button. Same entry point, so this is that repair. */
    set_remote_mouse_buttons(&global_state, 0x00);
    feed(ball, 0x00, 6, 0);
    check("the other board letting go, or saying so again, lets go here",
          queued_any && queued.buttons == 0x00, seen());

    /* What the heartbeat republishes is recomputed rather than remembered, so a device
       that went away stops being counted even though no report arrived to notice. */
    reset_state();
    keys = plug_in(1, 0);
    ball = plug_in(2, 0);

    feed(keys, 0x01, 0, 0);
    check("the union counts a button held anywhere on this board",
          refresh_local_mouse_buttons(&global_state) == 0x01, seen());

    unplug(keys);
    check("and stops counting it the moment that device is gone",
          refresh_local_mouse_buttons(&global_state) == 0x00, seen());
    check("and what the host is told drops it at the same moment",
          global_state.mouse_buttons == 0x00, seen());

    printf("\n  handing the cursor over\n\n");

    /* This board refuses to hand the cursor to the other computer while a button is held.
       First with the button on a device attached here, including the case the union added:
       the device that is moving holds nothing and another one here does. */
    reset_state();
    keys = plug_in(1, 0);
    ball = plug_in(2, 0);

    feed(keys, 0x01, 0, 0);
    global_state.pointer_x = MIN_SCREEN_COORD;
    feed(ball, 0x00, -40, 0);
    check("a button held on another device here holds the edge switch back",
          global_state.active_output == OUTPUT_A, seen());

    feed(keys, 0x00, 0, 0);
    global_state.pointer_x = MIN_SCREEN_COORD;
    feed(ball, 0x00, -40, 0);
    check("and the switch goes through once that device lets go",
          global_state.active_output == OUTPUT_B, seen());

    /* The same guard, for a button held on the other board. It can only honour that if it
       was told, which is what the announcement above is for. */
    reset_state();
    ball = plug_in(2, 0);
    global_state.pointer_x = MIN_SCREEN_COORD;
    set_remote_mouse_buttons(&global_state, 0x01);

    feed(ball, 0x00, -40, 0);
    check("a button held on the other board holds the edge switch back",
          global_state.active_output == OUTPUT_A, seen());

    set_remote_mouse_buttons(&global_state, 0x00);
    global_state.pointer_x = MIN_SCREEN_COORD;
    feed(ball, 0x00, -40, 0);
    check("and the switch goes through once it lets go",
          global_state.active_output == OUTPUT_B, seen());

    /* What this board forwards when the other one is the active output. */
    reset_state();
    global_state.active_output = OUTPUT_B;
    keys = plug_in(1, 0);

    feed(keys, 0x01, 0, 0);
    check("nothing is queued locally when the other board is active", !queued_any, seen());
    check("the forwarded report carries the button",
          forwarded_any && forwarded.buttons == 0x01, seen());

    /* Which endpoint the drain task waits on.

       A relative report goes out on its own interface and an absolute one shares
       interface 0 with the keyboard, consumer and system report IDs. The readiness check
       used to name interface 0 outright, so a relative report waited on an endpoint it
       was never going to be written to, and any keyboard traffic stalled it for no
       reason. Both sides now ask mouse_report_instance(), so the endpoint waited on is
       the endpoint written by construction. */
    printf("\n  waiting on the right endpoint\n\n");

    reset_state();

    check("a relative report waits on the relative interface",
          drain(RELATIVE) == ITF_NUM_HID_REL_M, seen());
    check("and that is where it was written",
          wrote_instance == ITF_NUM_HID_REL_M, seen());

    check("an absolute report waits on the shared interface",
          drain(ABSOLUTE) == ITF_NUM_HID, seen());
    check("and that is where it was written",
          wrote_instance == ITF_NUM_HID, seen());

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}

/* ==================================================== *
 * Stand-ins for everything mouse.c reaches for
 * ==================================================== */

device_t global_state = {0};

/* Two queues come through here. The mouse queue is what the host is told; the UART queue
   only carries this board's idea of where the cursor is, which no case below asserts on. */
bool queue_try_add(queue_t *queue, const void *value) {
    if (queue == &global_state.mouse_queue) {
        memcpy(&queued, value, sizeof(queued));
        queued_any = true;
    }

    return true;
}

/* Staged by the drain-side cases below; empty otherwise, which is what every other case
   here wants. */
mouse_report_t staged;
bool           staged_any = false;

bool queue_try_peek(queue_t *queue, void *value) {
    if (queue != &global_state.mouse_queue || !staged_any)
        return false;

    memcpy(value, &staged, sizeof(staged));
    return true;
}

bool queue_try_remove(queue_t *queue, void *value) { (void)queue; (void)value; return false; }

void queue_packet(const uint8_t *data, enum packet_type_e packet_type, int length) {
    if (packet_type != MOUSE_REPORT_MSG)
        return;

    memcpy(&forwarded, data, length < (int)sizeof(forwarded) ? (size_t)length : sizeof(forwarded));
    forwarded_any = true;
}

void send_value(const uint8_t value, enum packet_type_e packet_type) {
    if (packet_type != MOUSE_BUTTONS_MSG)
        return;

    sent_buttons = value;
    sent_count++;
}

uint64_t time_us_64(void) { return 0; }

/* Only the button field answers, so a wide value cannot be mistaken for movement. Which
   field this is comes from the pointer, since every mouse_t member is a report_val_t. */
int32_t get_report_value(uint8_t *report, int len, report_val_t *val) {
    (void)report; (void)len;

    for (int dev = 0; dev < MAX_DEVICES; dev++)
        for (int idx = 0; idx < MAX_INTERFACES; idx++)
            if (val == &global_state.iface[dev][idx].mouse.buttons)
                return wide_buttons;

    return 0;
}

void set_active_output(device_t *state, uint8_t new_output) {
    state->active_output = new_output;
}

bool tud_suspended(void) { return false; }
bool tud_remote_wakeup(void) { return false; }

/* Which interface process_mouse_queue_task() asked about, and which one the report was
   then written to. The whole point of the drain-side test is that these agree. */
uint8_t asked_instance = 0xFF;
uint8_t wrote_instance = 0xFF;

bool tud_hid_n_ready(uint8_t instance) {
    asked_instance = instance;
    return true;
}

uint8_t tud_hid_n_get_protocol(uint8_t instance) { (void)instance; return 1; }

bool tud_mouse_report(uint8_t mode, uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan) {
    (void)buttons; (void)x; (void)y; (void)wheel; (void)pan;
    wrote_instance = mouse_report_instance(mode);
    return true;
}
