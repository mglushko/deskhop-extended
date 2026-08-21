#!/usr/bin/python3

from dataclasses import dataclass, field

@dataclass
class FormField:
    offset: int
    name: str
    default: int | None = None
    values: dict[int, str] = field(default_factory=dict)
    data_type: str = "int32"
    elem: str | None = None

# Modifier bits and the handful of HID usages the compiled-in combos name, so the
# defaults below read as the combos they are. Everything else the page can capture is
# named in script.js, which has to turn a keypress into the same numbers.
MOD_LCTRL, MOD_LSHIFT, MOD_LALT, MOD_LGUI = 0x01, 0x02, 0x04, 0x08
MOD_RCTRL, MOD_RSHIFT, MOD_RALT, MOD_RGUI = 0x10, 0x20, 0x40, 0x80

KEY_A, KEY_B, KEY_C = 0x04, 0x05, 0x06
KEY_G, KEY_J, KEY_K, KEY_L = 0x0A, 0x0D, 0x0E, 0x0F
KEY_O, KEY_S, KEY_X, KEY_Y = 0x12, 0x16, 0x1B, 0x1C
KEY_CAPS_LOCK, KEY_F12 = 0x39, 0x45


def combo(modifier, first=0, second=0):
    """One hotkey packed the way config_t stores it - see HOTKEY_PACK in
    src/include/keyboard.h."""
    return modifier | (first << 8) | (second << 16)

STATUS_ = [
    FormField(78, "Running FW version", None, {}, "uint16", elem="fw_version"),
    FormField(79, "Running FW checksum", None, {}, "uint32", elem="hex_info"),
    FormField(86, "Other board FW version", None, {}, "uint16", elem="fw_version"),
]

CONFIG_ = [
    FormField(1001, "Mouse", elem="label"),
    FormField(71, "Force Mouse Boot Mode", None, {}, "uint8", "checkbox"),
    FormField(75, "Enable Acceleration", None, {}, "uint8", "checkbox"),
    FormField(77, "Jump Threshold", 0, {"min": 0, "max": 3000}, "uint16", "range"),
    FormField(83, "Edge Double-Tap to Switch", None, {}, "uint8", "checkbox"),
    # Not 0: a zero window makes window_us 0 in edge_double_tap_ready(), so the second
    # tap can never land inside it and output switching stops working altogether.
    FormField(84, "Double-Tap Time (ms)", 300, {"min": 50, "max": 2000}, "uint16", "range"),
    # Kept far below 16384, where the release check in process_mouse_report() can no
    # longer be satisfied by any pointer_x and the edge never releases.
    FormField(85, "Double-Tap Pull-Back Distance", 1000, {"min": 50, "max": 5000}, "uint16", "range"),

    FormField(1002, "Keyboard", elem="label"),
    FormField(72, "Force KBD Boot Protocol", None, {}, "uint8", "checkbox"),
    FormField(73, "KBD LED as Indicator", None, {}, "uint8", "checkbox"),

    FormField(76, "Enforce Ports", None, {}, "uint8", "checkbox"),

    # Config page layout only - which output is drawn in the left-hand column.
    FormField(87, "Swap Columns", 1, {}, "uint8", "checkbox"),

    FormField(1004, "Status LED", elem="label"),
    FormField(88, "Status LED", 0,
              {0: "Always on", 1: "When idle", 2: "After switching", 3: "Idle + switch"}, "uint8"),
    # Seconds here, not microseconds: both of these are stored in seconds.
    FormField(89, "Status LED Idle Time", 30, {"min": 1, "max": 4200}, "uint16"),
    FormField(103, "Status LED Switch Time", 10, {"min": 1, "max": 4200}, "uint16"),
]

# One row per entry in hotkeys[] (src/keyboard.c), in that order - the firmware stores
# them as an array and matches by position, so these cannot be reordered. The combo in
# `values` is what that entry is compiled with; the page shows it for a hotkey that has
# nothing stored, and a stored zero means exactly that. Keep them in step with the table
# in src/keyboard.c.
HOTKEYS_ = [
    FormField(90, "Switch output", 0,
              {"combo": combo(MOD_LCTRL, KEY_CAPS_LOCK)}, "uint32", "hotkey"),
    FormField(91, "Slow mouse", 0,
              {"combo": combo(MOD_RALT | MOD_RCTRL)}, "uint32", "hotkey"),
    FormField(92, "Lock switching", 0,
              {"combo": combo(MOD_RCTRL, KEY_K)}, "uint32", "hotkey"),
    FormField(93, "Lock both screens", 0,
              {"combo": combo(MOD_RCTRL, KEY_L)}, "uint32", "hotkey"),
    FormField(94, "Gaming mode", 0,
              {"combo": combo(MOD_LCTRL | MOD_RSHIFT, KEY_G)}, "uint32", "hotkey"),
    FormField(95, "Keep awake: pong", 0,
              {"combo": combo(MOD_LCTRL | MOD_RSHIFT, KEY_S)}, "uint32", "hotkey"),
    FormField(96, "Keep awake: jitter", 0,
              {"combo": combo(MOD_LCTRL | MOD_RSHIFT, KEY_J)}, "uint32", "hotkey"),
    FormField(97, "Keep awake: off", 0,
              {"combo": combo(MOD_LCTRL | MOD_RSHIFT, KEY_X)}, "uint32", "hotkey"),
    FormField(99, "Record screen alignment", 0,
              {"combo": combo(MOD_RSHIFT, KEY_F12, KEY_Y)}, "uint32", "hotkey"),
    FormField(100, "Config mode", 0,
              {"combo": combo(MOD_LCTRL | MOD_RSHIFT, KEY_C, KEY_O)}, "uint32", "hotkey"),
    FormField(101, "Firmware upgrade, board A", 0,
              {"combo": combo(MOD_LSHIFT | MOD_RSHIFT, KEY_A)}, "uint32", "hotkey"),
    FormField(102, "Firmware upgrade, board B", 0,
              {"combo": combo(MOD_LSHIFT | MOD_RSHIFT, KEY_B)}, "uint32", "hotkey"),
]

OUTPUT_ = [
    FormField(1, "Screen Count", 1, {1: "1", 2: "2", 3: "3"}, "uint32"),
    FormField(2, "Speed X", 16, {"min": 1, "max": 100}, "int32", "range"),
    FormField(3, "Speed Y", 16, {"min": 1, "max": 100}, "int32", "range"),
    FormField(4, "Border Top", None, {}, "int32"),
    FormField(5, "Border Bottom", None, {}, "int32"),
    FormField(6, "Operating System", 1, {1: "Linux", 2: "MacOS", 3: "Windows", 4: "Android", 255: "Other"}, "uint8"),
    FormField(7, "Screen Position", 1, {1: "Left", 2: "Right"}, "uint8"),
    FormField(8, "Cursor Park Position", 0, {0: "Top", 1: "Bottom", 3: "Previous"}, "uint8"),
    FormField(1003, "Screensaver", elem="label"),
    FormField(9, "Mode", 0, {0: "Disabled", 1: "Pong", 2: "Jitter"}, "uint8"),
    FormField(10, "Only If Inactive", None, {}, "uint8", "checkbox"),
    FormField(11, "Idle Time (μs)", None, {}, "uint64"),
    FormField(12, "Max Time (μs)", None, {}, "uint64"),
]

def generate_output(base, data):
    output = [
        {
            "name": field.name,
            "key": base + field.offset,
            "default": field.default,
            "values": field.values,
            "type": field.data_type,
            "elem": field.elem,
        }
        for field in data
    ]
    return output

def output_A(base=10):
    return generate_output(base, data=OUTPUT_)

def output_B(base=40):
    return generate_output(base, data=OUTPUT_)

def output_status():
    return generate_output(0, data=STATUS_)

def output_config():
    return generate_output(0, data=CONFIG_)


def output_hotkeys():
    return generate_output(0, data=HOTKEYS_)
