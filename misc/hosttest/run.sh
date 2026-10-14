#!/usr/bin/env bash
# Build and run the host-side tests. See test_config_store.c, test_hotkeys.c,
# test_save_shortcuts.c, test_mouse.c, test_wrap_around.c and test_uniform_speed.c.
set -euo pipefail
cd "$(dirname "$0")"
root="$(git rev-parse --show-toplevel)"
out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT

# shim/sdk stands in for the Pico SDK and TinyUSB where the firmware headers need a type
# from them. The headers below are only reached for, never read: main.h includes them and
# nothing any test calls goes near what they declare. Generated rather than committed,
# so an empty file that has to exist is not mistaken for one that says something.
for header in pio_usb.h hardware/dma.h hardware/uart.h \
              hardware/structs/ioqspi.h hardware/structs/sio.h hardware/sync.h \
              pico/multicore.h pico/unique_id.h; do
    mkdir -p "$out/sdk/$(dirname "$header")"
    echo '#pragma once' > "$out/sdk/$header"
done

# Packet validation has no hardware dependencies, so exercise the real config endpoint
# allowlist directly before the broader host-side tests.
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim -I shim/sdk -I "$root/src/include" \
    test_packet_validation.c "$root/src/packet_validation.c" \
    -o "$out/test_packet_validation"

# test_config_store keeps its own main.h, which stubs hid_interface_t so it stays clear of
# hid_parser.h. test_hotkeys wants the real interface and keyboard types, so it builds
# against the firmware's own main.h with the generated headers underneath - which is why
# shim/ is not on its include path and shim/sdk is.
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim -I shim/sdk -I "$root/src/include" \
    test_config_store.c "$root/src/config_store.c" "$root/src/constants.c" \
    "$root/src/protocol.c" -o "$out/test_config_store"

gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim/sdk -I "$out/sdk" -I "$root/src/include" \
    test_hotkeys.c "$root/src/keyboard.c" "$root/src/constants.c" \
    "$root/src/protocol.c" -o "$out/test_hotkeys"

# The same table again, reached the way the config page reaches it: through handlers.c,
# which is why that file's handlers are linked here rather than stubbed as test_hotkeys does.
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim/sdk -I "$out/sdk" -I "$root/src/include" \
    test_save_shortcuts.c "$root/src/handlers.c" "$root/src/keyboard.c" \
    "$root/src/constants.c" "$root/src/protocol.c" -o "$out/test_save_shortcuts"

# mouse.c on its own: the button combining it does reaches nothing outside this file, and
# -lm is for the acceleration curve. Same include path as test_hotkeys, same reasons.
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim/sdk -I "$out/sdk" -I "$root/src/include" \
    test_mouse.c "$root/src/mouse.c" -lm -o "$out/test_mouse"

# The same file again for the outer edge switch, with its own stand-ins for what it reaches.
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim/sdk -I "$out/sdk" -I "$root/src/include" \
    test_wrap_around.c "$root/src/mouse.c" -lm -o "$out/test_wrap_around"

# And once more for uniform speed, the same file's arithmetic with its own settings.
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim/sdk -I "$out/sdk" -I "$root/src/include" \
    test_uniform_speed.c "$root/src/mouse.c" -lm -o "$out/test_uniform_speed"

"$out/test_packet_validation"
"$out/test_config_store"
"$out/test_hotkeys"
"$out/test_save_shortcuts"
"$out/test_mouse"
"$out/test_wrap_around"
"$out/test_uniform_speed"
