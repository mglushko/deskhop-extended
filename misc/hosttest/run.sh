#!/usr/bin/env bash
# Build and run the host-side config format tests. See test_config_store.c.
set -euo pipefail
cd "$(dirname "$0")"
root="$(git rev-parse --show-toplevel)"
out="$(mktemp -d)"; trap 'rm -rf "$out"' EXIT
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined \
    -I shim -I "$root/src/include" \
    test_config_store.c "$root/src/config_store.c" "$root/src/constants.c" "$root/src/protocol.c" \
    -o "$out/test_config_store"
"$out/test_config_store"
