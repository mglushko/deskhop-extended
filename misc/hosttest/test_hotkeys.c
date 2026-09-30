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

/* The first entry that is on and does not answer its own combination, or -1. This is what
   a shortcut left dead looks like from the keyboard: pressing it runs something else. */
static int first_dead(void) {
    for (int n = 0; n < NUM_HOTKEYS; n++)
        if (!hotkeys[n].disabled
            && matched(hotkeys[n].modifier, hotkeys[n].keys[0], hotkeys[n].keys[1], 0) != n)
            return n;

    return -1;
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

    /* One key and no modifier would take that key away from everything typed, since a
       report a shortcut answers is swallowed. Neither slot, nor both holding the same key,
       gets it past. */
    {
        static const uint32_t lone[] = {
            HOTKEY_PACK(0, HID_KEY_F1, HID_KEY_NONE),
            HOTKEY_PACK(0, HID_KEY_NONE, HID_KEY_F1),
            HOTKEY_PACK(0, HID_KEY_F1, HID_KEY_F1),
        };
        int kept = 0;

        for (unsigned v = 0; v < ARRAY_SIZE(lone); v++) {
            clear_config();
            global_state.config.hotkey_cfg[5] = lone[v];
            hotkeys_apply_config(&global_state);
            kept += global_state.config.hotkey_cfg[5] != 0;
        }

        snprintf(detail, sizeof(detail), "%d of %d kept", kept, (int)ARRAY_SIZE(lone));
        check("a single key with no modifier is not stored", kept == 0, detail);
        check("and that entry is back to the combo it was built with",
              combo_is(5, KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                       HID_KEY_S, 0), combo_str(5));
    }

    clear_config();
    global_state.config.hotkey_cfg[5] = HOTKEY_PACK(0, HID_KEY_F1, HID_KEY_F12);
    hotkeys_apply_config(&global_state);
    check("two keys with no modifier may still be stored",
          combo_is(5, 0, HID_KEY_F1, HID_KEY_F12), combo_str(5));

    clear_config();
    global_state.config.hotkey_cfg[5] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_F1,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("as may one key with a modifier",
          combo_is(5, KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_F1, 0), combo_str(5));

    /* Byte 3 of the packed word carries nothing. A value that is non-zero only there is not
       the "use the default" sentinel, and unpacks to no modifier and no key. */
    clear_config();
    global_state.config.hotkey_cfg[2] = 0x01000000;
    hotkeys_apply_config(&global_state);
    check("a value that says nothing but is not zero is cleared",
          global_state.config.hotkey_cfg[2] == 0, combo_str(2));
    check("and that entry is back to the combo it was built with",
          combo_is(2, KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0), combo_str(2));

    /* HOTKEY_OFF sits in that same byte and has to survive the clear above, or turning a
       shortcut off would put it back on the combination it was built with instead. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_OFF;
    hotkeys_apply_config(&global_state);
    check("a shortcut turned off stays turned off",
          global_state.config.hotkey_cfg[2] == HOTKEY_OFF, combo_str(2));
    check("and the entry says so", hotkeys[2].disabled, combo_str(2));
    check("while the ones either side of it do not",
          !hotkeys[1].disabled && !hotkeys[3].disabled, combo_str(3));

    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_Y,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("storing a combination against it turns it back on",
          !hotkeys[2].disabled
          && combo_is(2, KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_Y, 0), combo_str(2));

    clear_config();
    global_state.config.hotkey_cfg[HOTKEY_CONFIG_IDX] = HOTKEY_OFF;
    hotkeys_apply_config(&global_state);
    check("config mode cannot be turned off either",
          global_state.config.hotkey_cfg[HOTKEY_CONFIG_IDX] == 0
          && !hotkeys[HOTKEY_CONFIG_IDX].disabled,
          combo_str(HOTKEY_CONFIG_IDX));

    /* hotkey_toggle is honoured only where nothing is stored, and off is stored. Otherwise
       turning the switch shortcut off on a board carrying a non-default toggle key would
       quietly put it back under that key instead of turning it off. */
    clear_config();
    global_state.config.hotkey_toggle = HID_KEY_F1;
    global_state.config.hotkey_cfg[0] = HOTKEY_OFF;
    hotkeys_apply_config(&global_state);
    check("off wins over the legacy toggle key", hotkeys[0].disabled, combo_str(0));
    check("and neither combination answers",
          matched(HOTKEY_MODIFIER, HID_KEY_F1, 0, 0) == -1
          && matched(HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0, 0) == -1, combo_str(0));

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

    /* hotkey_toggle is a stored combination too, and held to the same rules. Left Ctrl + G
       is part of Gaming mode's Left Ctrl + Right Shift + G, which it is asked ahead of. */
    clear_config();
    global_state.config.hotkey_toggle = HID_KEY_G;
    hotkeys_apply_config(&global_state);
    check("a toggle key that would leave another entry dead is not honoured",
          combo_is(0, HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0), combo_str(0));
    check("and goes back to the key this firmware was built with",
          global_state.config.hotkey_toggle == HOTKEY_TOGGLE, "");
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G, 0, 0));
    check("so Gaming mode still answers its own",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_G, 0, 0) == 4, detail);

    /* Once Gaming mode has moved off it, the same key is free. */
    clear_config();
    global_state.config.hotkey_toggle = HID_KEY_G;
    global_state.config.hotkey_cfg[4] = HOTKEY_PACK(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_G,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("but is honoured once the entry it clashed with has moved",
          combo_is(0, HOTKEY_MODIFIER, HID_KEY_G, 0)
          && global_state.config.hotkey_toggle == HID_KEY_G, combo_str(0));

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

    /* An entry that is off is not standing for anything, so what it held is free as well. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_OFF;
    global_state.config.hotkey_cfg[3] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("a combo held by an entry that is off can be taken",
          combo_is(3, KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0), combo_str(3));

    printf("\n  one combination inside another\n\n");

    /* A report goes to the first entry whose modifiers and keys it holds, and it may hold
       more. So an entry set to a combination that contains one asked ahead of it would
       never fire: pressing it runs the other. Entry 3, Lock both screens, is Right Ctrl + L
       and is asked ahead of entry 4, Gaming mode. */
    clear_config();
    global_state.config.hotkey_cfg[4] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_RIGHTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_L, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("a combo containing one asked ahead of it is not stored",
          global_state.config.hotkey_cfg[4] == 0, combo_str(4));
    check("and that entry is back to the combo it was built with",
          combo_is(4, KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G, 0),
          combo_str(4));

    /* The same from the other side: set above an entry that cannot move, to part of its
       combination. Entry 4 is Left Ctrl + Right Shift + G, with nothing stored. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("nor is part of the combo of an entry below it",
          global_state.config.hotkey_cfg[2] == 0, combo_str(2));
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G, 0, 0));
    check("and the entry below still answers its own",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_G, 0, 0) == 4, detail);

    /* Config mode is asked ahead of everything, wherever it sits in the table. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT | KEYBOARD_MODIFIER_LEFTALT,
        HID_KEY_C, HID_KEY_O);
    hotkeys_apply_config(&global_state);
    check("nor a combo containing the config mode one",
          global_state.config.hotkey_cfg[2] == 0, combo_str(2));

    /* Part of the combination of an entry asked ahead of it is fine: that entry needs more
       than this one's combination holds, and gets its own first. Entry 5 is Keep awake:
       pong, below Gaming mode. */
    clear_config();
    global_state.config.hotkey_cfg[5] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G,
                                                    HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("part of an entry above it may be stored",
          combo_is(5, KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G, 0), combo_str(5));
    check("and each of the two answers its own",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_G, 0, 0) == 4
          && matched(KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_G, 0, 0) == 5, "");

    /* Slow mouse names no key, so it is held back until no entry that named one answered,
       and neither takes a combination built on its modifiers nor loses its own. */
    clear_config();
    global_state.config.hotkey_cfg[5] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_S, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("a combo built on slow mouse's modifiers may be stored",
          combo_is(5, KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_S, 0),
          combo_str(5));
    check("and both still answer their own",
          matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL,
                  HID_KEY_S, 0, 0) == 5
          && matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, 0, 0, 0) == 1,
          "");

    /* Nor when it is slow mouse that moves, onto modifiers another entry's combination is
       built on. Entry 4, Gaming mode, is Left Ctrl + Right Shift + G. */
    clear_config();
    global_state.config.hotkey_cfg[1] = HOTKEY_PACK(
        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_NONE, HID_KEY_NONE);
    hotkeys_apply_config(&global_state);
    check("slow mouse may be set to modifiers another entry is built on",
          combo_is(1, KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, 0, 0),
          combo_str(1));
    check("and both still answer their own",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, 0, 0, 0) == 1
          && matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                     HID_KEY_G, 0, 0) == 4, "");

    /* A key named twice is that key once, as far as the matcher is concerned. */
    clear_config();
    global_state.config.hotkey_cfg[5] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_L,
                                                    HID_KEY_L);
    hotkeys_apply_config(&global_state);
    check("naming a key twice does not get a taken combo past the check",
          global_state.config.hotkey_cfg[5] == 0, combo_str(5));

    /* A refused entry goes back to the combination it was built with, which an entry decided
       earlier on the same pass took to be moving away. Entry 5's stored combo contains Lock
       both screens and is refused; entry 2's is part of the combo entry 5 then goes back
       to, so it has to go as well. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_S,
                                                    HID_KEY_NONE);
    global_state.config.hotkey_cfg[5] = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_L,
                                                    HID_KEY_Q);
    hotkeys_apply_config(&global_state);
    check("an entry refused onto its built-in combo takes it back from one decided earlier",
          global_state.config.hotkey_cfg[5] == 0 && global_state.config.hotkey_cfg[2] == 0,
          combo_str(2));
    snprintf(detail, sizeof(detail), "entry %d", first_dead());
    check("and no entry is left without its own combination", first_dead() == -1, detail);

    /* Every settable entry set, one at a time, to each combination one modifier or one key
       more or fewer than another entry's, to that combination itself with its keys either
       way round, and to it with its key named twice. Whatever is kept, no entry that is on
       may be left without its own. */
    {
        hotkey_combo_t built[NUM_HOTKEYS];
        static const uint8_t extra_keys[] = {HID_KEY_Q, HID_KEY_L, HID_KEY_G, HID_KEY_O};
        int tried = 0, dead = 0;
        char first[64] = "none", summary[128];

        clear_config();
        hotkeys_apply_config(&global_state);
        memcpy(built, hotkeys, sizeof(built));
        snprintf(detail, sizeof(detail), "entry %d", first_dead());
        check("as built, every entry answers its own combination", first_dead() == -1, detail);

        for (int n = 0; n < NUM_HOTKEYS; n++) {
            if (n == HOTKEY_CONFIG_IDX)
                continue;

            for (int m = 0; m < NUM_HOTKEYS; m++) {
                uint32_t variants[16];
                int count = 0;
                uint8_t mod = built[m].modifier, k1 = built[m].keys[0], k2 = built[m].keys[1];

                if (m == n)
                    continue;

                variants[count++] = HOTKEY_PACK(mod, k1, k2);

                for (int bit = 0; bit < 8; bit++)
                    variants[count++] = HOTKEY_PACK(mod ^ (1 << bit), k1, k2);

                if (k2) {
                    variants[count++] = HOTKEY_PACK(mod, k2, k1);
                    variants[count++] = HOTKEY_PACK(mod, k1, HID_KEY_NONE);
                    variants[count++] = HOTKEY_PACK(mod, k2, HID_KEY_NONE);
                }

                if (!k2)
                    for (unsigned e = 0; e < ARRAY_SIZE(extra_keys); e++)
                        if (extra_keys[e] != k1)
                            variants[count++] = HOTKEY_PACK(mod, k1 ? k1 : extra_keys[e],
                                                            k1 ? extra_keys[e] : HID_KEY_NONE);

                if (k1 && !k2)
                    variants[count++] = HOTKEY_PACK(mod, k1, k1);

                for (int v = 0; v < count; v++) {
                    clear_config();
                    global_state.config.hotkey_cfg[n] = variants[v];
                    hotkeys_apply_config(&global_state);
                    tried++;

                    if (first_dead() != -1 && dead++ == 0)
                        snprintf(first, sizeof(first), "entry %d stored %08x kills %d", n,
                                 variants[v], first_dead());
                }
            }
        }

        snprintf(summary, sizeof(summary), "%d of %d, first: %s", dead, tried, first);
        check("no single stored combination leaves an entry without its own", dead == 0,
              summary);
        printf("    (%d combinations tried)\n", tried);
    }

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

    /* Turned off, and the report goes past it - which is the whole point, since a shortcut
       that answers is swallowed and never reaches the computer. */
    clear_config();
    global_state.config.hotkey_cfg[2] = HOTKEY_OFF;
    hotkeys_apply_config(&global_state);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0));
    check("an entry that is off answers nothing",
          matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0) == -1, detail);

    /* The one built without a key is the fallback for its modifiers; off, it is not. */
    clear_config();
    global_state.config.hotkey_cfg[1] = HOTKEY_OFF;
    hotkeys_apply_config(&global_state);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, 0, 0, 0));
    check("nor does the keyless one once it is off",
          matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, 0, 0, 0) == -1,
          detail);

    /* Every settable entry off at once, which is as far as this goes, and the way back is
       still there. */
    clear_config();
    for (int n = 0; n < NUM_HOTKEYS; n++)
        global_state.config.hotkey_cfg[n] = HOTKEY_OFF;
    hotkeys_apply_config(&global_state);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                     HID_KEY_C, HID_KEY_O, 0));
    check("config mode still answers with everything else turned off",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_C, HID_KEY_O, 0) == HOTKEY_CONFIG_IDX, detail);

    printf("\n  while the config page is open\n\n");

    /* The page records a shortcut from the keystrokes that reach it, so while this board is
       in config mode and typing into the page's computer, a combination it already has must
       reach the page rather than run. Each of these used to run instead, the first by
       switching the keyboard to the other computer, and the page, which never saw the key
       that completed one, kept whatever had got through. */
    clear_config();
    hotkeys_apply_config(&global_state);
    global_state.config_mode_active = true;
    global_state.board_role = OUTPUT_A;
    global_state.active_output = OUTPUT_A;

    snprintf(detail, sizeof(detail), "entry %d", matched(HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0, 0));
    check("the switch combination reaches the page",
          matched(HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0, 0) == -1, detail);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0));
    check("so does any other the board has",
          matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0) == -1, detail);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_K, 0, 0));
    check("and a new combination that merely contains one",
          matched(KEYBOARD_MODIFIER_RIGHTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_K, 0, 0) == -1, detail);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_S, 0, 0));
    check("the modifiers slow mouse answers on its own do not toggle it",
          matched(KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL,
                  HID_KEY_S, 0, 0) == -1, detail);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_A, 0, 0));
    check("nor do both shifts and A drop the board into BOOTSEL",
          matched(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_A, 0, 0) == -1, detail);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                     HID_KEY_C, HID_KEY_O, 0));
    check("config mode still answers, which is the way out",
          matched(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
                  HID_KEY_C, HID_KEY_O, 0) == HOTKEY_CONFIG_IDX, detail);

    /* Typing into the other computer, nothing can be recording, so the table is back. That
       keeps Switch output as a way back to the page. */
    global_state.active_output = OUTPUT_B;
    snprintf(detail, sizeof(detail), "entry %d", matched(HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0, 0));
    check("typing into the other computer, switch output answers",
          matched(HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0, 0) == 0, detail);
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0));
    check("and so does the rest of the table",
          matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0) == 2, detail);

    global_state.config_mode_active = false;
    global_state.active_output = OUTPUT_A;
    snprintf(detail, sizeof(detail), "entry %d",
             matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0));
    check("out of config mode nothing is held back",
          matched(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0, 0) == 2
          && matched(HOTKEY_MODIFIER, HOTKEY_TOGGLE, 0, 0) == 0, detail);

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
