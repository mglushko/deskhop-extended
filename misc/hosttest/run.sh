#!/usr/bin/env bash
# Build and run the host-side tests. See test_config_store.c and test_hotkeys.c.
set -euo pipefail
cd "$(dirname "$0")"
root="$(git rev-parse --show-toplevel)"
out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT

# shim/sdk stands in for the Pico SDK and TinyUSB where the firmware headers need a type
# from them. The headers below are only reached for, never read: main.h includes them and
# nothing either test calls goes near what they declare. Generated rather than committed,
# so an empty file that has to exist is not mistaken for one that says something.
for header in pio_usb.h hardware/dma.h hardware/uart.h hardware/watchdog.h \
              hardware/structs/ioqspi.h hardware/structs/sio.h hardware/sync.h \
              pico/bootrom.h pico/multicore.h pico/unique_id.h; do
    mkdir -p "$out/sdk/$(dirname "$header")"
    echo '#pragma once' > "$out/sdk/$header"
done

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

"$out/test_config_store"
"$out/test_hotkeys"
