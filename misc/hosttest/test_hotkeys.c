/* Host-side tests for the hotkey table in src/keyboard.c.
 *
 * A shortcut is stored as a packed uint32 that arrives from the config page with no
 * validation of any kind (handlers.c memcpys it straight into config.hotkey_cfg), and two
 * rules decide what it then means: hotkeys_apply_config refuses what cannot be honoured,
 * and check_all_hotkeys decides which entry answers a report. Both fail silently when they
 * are wrong, and the way they were wrong could take the config page out of reach - which
 * is also the only way to put a shortcut back. So they are checked here, off-device.
 *
 * Built and run by run.sh. Everything below the tests is a stand-in for the parts of the
 * firmware keyboard.c links against but none of this exercises. */
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

/* What hotkeys[n] holds, as a string, so a failure says what it found. */
static const char *combo_str(int n) {
    static char buf[64];
    int len = snprintf(buf, sizeof(buf), "mod=%02x keys=%d", hotkeys[n].modifier,
                       hotkeys[n].key_count);

    for (int k = 0; k < hotkeys[n].key_count && len < (int)sizeof(buf); k++)
        len += snprintf(buf + len, sizeof(buf) - len, " %02x", hotkeys[n].keys[k]);

    return buf;
}

static bool combo_is(int n, uint8_t modifier, uint8_t k1, uint8_t k2) {
    uint8_t count = (k1 ? 1 : 0) + (k2 ? 1 : 0);

    if (hotkeys[n].modifier != modifier || hotkeys[n].key_count != count)
        return false;

    return (!k1 || hotkeys[n].keys[0] == k1) && (!k2 || hotkeys[n].keys[1] == k2);
}

static hid_keyboard_report_t report_of(uint8_t modifier, uint8_t k1, uint8_t k2, uint8_t k3) {
    hid_keyboard_report_t report = {.modifier = modifier};

    report.keycode[0] = k1;
    report.keycode[1] = k2;
    report.keycode[2] = k3;

    return report;
}

/* Which entry answers this report, as an index, or -1. */
static int matched(uint8_t modifier, uint8_t k1, uint8_t k2, uint8_t k3) {
    hid_keyboard_report_t report = report_of(modifier, k1, k2, k3);
    hotkey_combo_t *hit = check_all_hotkeys(&report, &global_state);

    return hit ? (int)(hit - hotkeys) : -1;
}

static void clear_config(void) {
    memset(global_state.config.hotkey_cfg, 0, sizeof(global_state.config.hotkey_cfg));
    global_state.config.hotkey_toggle = HOTKEY_TOGGLE;
}

int main(void) {
    char detail[64];

    /* First call, and it has to be the one that captures the compiled-in table, so nothing
       may be stored before it. Every later case leans on that snapshot. */
    clear_config();
    hotkeys_apply_config(&global_state);

    printf("\n  the compiled-in table\n\n");

    check("nothing stored leaves the switch combo as built",
          combo_is(0, HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0), combo_str(0));
    check("the one entry built without a key of its own keeps none",
          combo_is(1, KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, 0, 0),
          combo_str(1));
    check("config mode is Left Ctrl + Right Shift + C + O",
          combo_is(HOTKEY_CONFIG_IDX,
                   KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                   HID_KEY_C, HID_KEY_O),
          combo_str(HOTKEY_CONFIG_IDX));

    printf("\n  what may be stored\n\n");

    /* The reported bug: modifiers with no key match every report holding them, and this
       entry is asked first, so it answered for everything. */
    clear_config();
    global_state.config.hotkey_cfg[0] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTCTRL,
                                                    HID_KEY_NONE, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    snprintf(detail, sizeof(detail), "stored %08x", global_state.config.hotkey_cfg[0]);
    check("a combo with modifiers and no key is not stored", global_state.config.hotkey_cfg[0] == 0,
          detail);
    check("and the entry is back to the combo it was built with",
          combo_is(0, HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0), combo_str(0));

    clear_config();
    global_state.config.hotkey_cfg[1] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_NONE, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("the entry built without a key may still be set to modifiers alone",
          combo_is(1, KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT, 0, 0),
          combo_str(1));

    /* Byte 3 of the packed word carries nothing. A value that is non-zero only there is not
       the "use the default" sentinel, and unpacks to no modifier and no key. */
    clear_config();
    global_state.config.hotkey_cfg[2] = 0x01000000;
    hotkeys_apply_config(&global_state);
    check("a value that says nothing but is not zero is cleared",
          global_state.config.hotkey_cfg[2] == 0, combo_str(2));
    check("and that entry is back to the combo it was built with",
          combo_is(2, KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0), combo_str(2));

    clear_config();
    global_state.config.hotkey_cfg[HOTKEY_CONFIG_IDX] =
        HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_Y, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("config mode is not settable at all",
          global_state.config.hotkey_cfg[HOTKEY_CONFIG_IDX] == 0,
          combo_str(HOTKEY_CONFIG_IDX));
    check("and stays on the combination this firmware was built with",
          combo_is(HOTKEY_CONFIG_IDX,
                   KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                   HID_KEY_C, HID_KEY_O),
          combo_str(HOTKEY_CONFIG_IDX));

    /* The refusal clears the stored value, which puts the entry back on the fallback chain
       rather than on the compiled-in combo directly - so the legacy hotkey_toggle still
       gets its say, on the refusing call and on every one after it alike. */
    clear_config();
    global_state.config.hotkey_toggle = HID_KEY_F1;
    hotkeys_apply_config(&global_state);
    check("hotkey_toggle still names the switch key when nothing is stored",
          combo_is(0, HOTKEY_MODIFIER, HID_KEY_F1, 0), combo_str(0));

    global_state.config.hotkey_cfg[0] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTALT,
                                                    HID_KEY_NONE, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("and a refused combo falls back to it, not past it",
          combo_is(0, HOTKEY_MODIFIER, HID_KEY_F1, 0), combo_str(0));

    printf("\n  duplicates\n\n");

    /* Two entries standing for one combination leaves the lower one dead, since the report
       never gets past the first that fits. Entry 2 is Right Ctrl + K. */
    clear_config();
    global_state.config.hotkey_cfg[3] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("a combo another entry already stands for is not stored",
          global_state.config.hotkey_cfg[3] == 0, combo_str(3));
    check("and that entry is back to the combo it was built with",
          combo_is(3, KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_L, 0), combo_str(3));

    /* Which slot holds which key is not part of what a combination is - the matcher asks
       only that each one is somewhere in the report. Entry 8 is Right Shift + F12 + Y. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_Y,
                                                    HID_KEY_F12);
    hotkeys_apply_config(&global_state);
    check("the same two keys the other way round is still the same combo",
          global_state.config.hotkey_cfg[2] == 0, combo_str(2));

    /* Config mode is fixed, so where it collides the stored one is always the one to go -
       even from an entry above it in the table, which is decided first. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_C, HID_KEY_O);
    hotkeys_apply_config(&global_state);
    check("nothing may be set to the config mode combination",
          global_state.config.hotkey_cfg[2] == 0, combo_str(2));
    check("and config mode still answers it",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_C, HID_KEY_O, 0) == HOTKEY_CONFIG_IDX, "");

    /* The check is against what the table ends up holding, not against the compiled list,
       so a combination the entry that held it has moved off is free to take. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_K,
                                                    HID_KEY_NONE);
    global_state.config.hotkey_cfg[3] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("a combo freed up by the entry that held it can be taken",
          combo_is(3, KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0), combo_str(3));

    printf("\n  which entry answers a report\n\n");

    clear_config();
    hotkeys_apply_config(&global_state);

    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, 0, 0, 0));
    check("modifiers alone reach the entry that asks for nothing else",
          matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, 0, 0, 0) == 1,
          detail);

    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL,
                     HID_KEY_K, 0, 0));
    check("a key that was actually pressed answers ahead of them",
          matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL,
                  HID_KEY_K, 0, 0) == 2, detail);

    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT,
                     HID_KEY_A, HID_KEY_B, 0));
    check("two entries that both fit are still taken in table order",
          matched(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_A, HID_KEY_B, 0) == 10, detail);

    /* The lockout this whole change is about, both ways round. */
    clear_config();
    global_state.config.hotkey_cfg[1] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTCTRL,
                                                    HID_KEY_NONE, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                     HID_KEY_C, HID_KEY_O, 0));
    check("a keyless combo stored above config mode does not swallow it",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_C, HID_KEY_O, 0) == HOTKEY_CONFIG_IDX, detail);

    /* An entry set to part of the config combination is asked first by table order, and
       still does not get it. */
    clear_config();
    global_state.config.hotkey_cfg[0] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_C, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                     HID_KEY_C, HID_KEY_O, 0));
    check("nor does one set to part of the config combination",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_C, HID_KEY_O, 0) == HOTKEY_CONFIG_IDX, detail);
    check("though it still answers on its own",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_C, 0, 0) == 0, detail);

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}

/* ---- what keyboard.c links against and none of the above reaches ---------------- */

device_t global_state;

void blink_led(device_t *state) { (void)state; }
void queue_packet(const uint8_t *data, enum packet_type_e type, int length) {
    (void)data; (void)type; (void)length;
}
int32_t extract_kbd_data(uint8_t *raw, int len, uint8_t itf, hid_interface_t *iface,
                         hid_keyboard_report_t *out) {
    (void)raw; (void)len; (void)itf; (void)iface; (void)out; return 0;
}
bool queue_try_add(queue_t *q, const void *v) { (void)q; (void)v; return true; }
bool queue_try_peek(queue_t *q, void *v) { (void)q; (void)v; return false; }
bool queue_try_remove(queue_t *q, void *v) { (void)q; (void)v; return false; }
uint64_t time_us_64(void) { return 0; }
void write_raw_packet(uint8_t *dst, uart_packet_t *packet) { (void)dst; (void)packet; }
bool tud_suspended(void) { return false; }
bool tud_remote_wakeup(void) { return false; }
bool tud_hid_n_ready(uint8_t instance) { (void)instance; return false; }
uint8_t tud_hid_n_get_protocol(uint8_t instance) { (void)instance; return 1; }
bool tud_hid_n_report(uint8_t instance, uint8_t report_id, const void *report, uint16_t len) {
    (void)instance; (void)report_id; (void)report; (void)len; return true;
}
bool tud_hid_keyboard_report(uint8_t report_id, uint8_t modifier, const uint8_t *keycode) {
    (void)report_id; (void)modifier; (void)keycode; return true;
}

/* The table takes the address of every one of these. */
void output_toggle_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void mouse_zoom_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void switchlock_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void screenlock_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void toggle_gaming_mode_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void enable_screensaver_pong_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void enable_screensaver_jitter_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void disable_screensaver_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void screen_border_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void config_enable_hotkey_handler(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void fw_upgrade_hotkey_handler_A(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
void fw_upgrade_hotkey_handler_B(device_t *s, hid_keyboard_report_t *r) { (void)s; (void)r; }
