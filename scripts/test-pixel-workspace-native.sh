#!/bin/sh
# Independent of world fixtures, CMake, Emscripten and generated Wasm artifacts.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/terra-pixel-native.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
if [ "${SANITIZE:-0}" = 1 ]; then
    set -- -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
else
    set -- -O2
fi
"${CC:-cc}" -std=c17 -Wall -Wextra -Werror -UNDEBUG -DTERRAX_TESTING "$@" \
    -I"$ROOT/include" "$ROOT/src/terra_pixel_workspace.c" "$ROOT/src/terra_mem.c" \
    "$ROOT/tests/pixel_workspace_contract.c" -o "$BUILD/pixel-workspace-contract"
"$BUILD/pixel-workspace-contract"
