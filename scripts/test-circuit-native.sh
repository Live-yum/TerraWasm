#!/bin/sh
# Runs the actual persistent allocator; independent of fixtures or Emscripten.
set -eu
CIRCUIT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CIRCUIT_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/terra-circuit-native.XXXXXX")
trap 'rm -rf "$CIRCUIT_BUILD"' EXIT HUP INT TERM
if [ "${SANITIZE:-0}" = 1 ]; then
    set -- -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
else
    set -- -O2
fi
"${CC:-cc}" -std=c17 -Wall -Wextra -Werror -UNDEBUG -DTERRAX_TESTING "$@" \
    -I"$CIRCUIT_ROOT/include" "$CIRCUIT_ROOT/src/terra_circuit.c" "$CIRCUIT_ROOT/src/terra_mem.c" \
    "$CIRCUIT_ROOT/tests/circuit_contract.c" -o "$CIRCUIT_BUILD/circuit-contract"
"$CIRCUIT_BUILD/circuit-contract"
