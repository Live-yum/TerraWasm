"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const WLD_PATH = path.resolve(__dirname, "..", "..", "TerraX", "wld", "1.wld");
const TXCI_PATH = path.resolve(__dirname, "..", "data", "terraria_color_index.txci");
const RECORD_CELLS = 64 * 64;
const RECORD_BYTES = 8 + RECORD_CELLS * 2;

function makeRecord({ cx = 0, cy = 0, used = 1, reserved = 0, fill } = {}) {
  const bytes = new Uint8Array(RECORD_BYTES);
  const view = new DataView(bytes.buffer);
  view.setUint16(0, cx, true);
  view.setUint16(2, cy, true);
  view.setUint16(4, used, true);
  view.setUint16(6, reserved, true);
  if (typeof fill === "function") fill(new Uint16Array(bytes.buffer, 8, RECORD_CELLS));
  else new Uint16Array(bytes.buffer, 8, RECORD_CELLS)[0] = 1;
  return bytes;
}

test("bulk indexed ABI rejects malformed records without leaking either heap", async (t) => {
  if (!fs.existsSync(WLD_PATH) || !fs.existsSync(TXCI_PATH)) {
    t.skip("pixel-art fixtures are unavailable");
    return;
  }

  const M = await TerraWorldWasm();
  assert.equal(typeof M._txw_add_pixel_art_chunks_bulk, "function");

  function alloc(bytes) {
    const value = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const ptr = M._tx_malloc(value.byteLength);
    assert.notEqual(ptr, 0);
    M.HEAPU8.set(value, ptr);
    return ptr;
  }

  const worldBytes = fs.readFileSync(WLD_PATH);
  const handlePtr = M._tx_malloc(4);
  const worldPtr = alloc(worldBytes);
  assert.equal(M._terra_world_open_from_buffer(worldPtr, worldBytes.byteLength, handlePtr), 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  M._tx_free(worldPtr);
  M._tx_free(handlePtr);

  const txci = fs.readFileSync(TXCI_PATH);
  const txciPtr = alloc(txci);
  const palette = Uint8Array.of(0, 0, 0, 0, 255, 0, 0, 255);
  const palettePtr = alloc(palette);
  assert.equal(M._txw_begin_pixel_art_indexed(
    handle,
    500, 500,
    16, 16,
    palettePtr, 2,
    txciPtr, txci.byteLength,
    0, 0,
    0, 0,
    0,
  ), 0);
  M._tx_free(palettePtr);
  M._tx_free(txciPtr);

  assert.equal(M._tx_bridge_heap_used(), 0);
  const nativeBaseline = M._tx_native_heap_used();

  function reject(bytes, length = bytes.byteLength, count = 1) {
    const ptr = alloc(bytes);
    const status = M._txw_add_pixel_art_chunks_bulk(handle, ptr, length, count);
    M._tx_free(ptr);
    assert.ok(status < 0);
    assert.equal(M._tx_bridge_heap_used(), 0);
    assert.equal(M._tx_native_heap_used(), nativeBaseline);
  }

  reject(makeRecord(), RECORD_BYTES - 1, 1);
  reject(makeRecord(), RECORD_BYTES, 2);
  reject(makeRecord(), RECORD_BYTES, 64);
  reject(makeRecord({ used: 4097 }));
  reject(makeRecord({ cx: 1 }));
  reject(makeRecord({ reserved: 1 }));
  reject(makeRecord({ used: 1, fill(indices) { indices[16] = 1; } }));
  reject(makeRecord({ used: 1, fill(indices) { indices[0] = 2; } }));

  const valid = makeRecord({
    used: 16 * 16,
    fill(indices) {
      for (let y = 0; y < 16; y += 1) {
        for (let x = 0; x < 16; x += 1) indices[y * 64 + x] = 1;
      }
    },
  });
  const validPtr = alloc(valid);
  assert.equal(M._txw_add_pixel_art_chunks_bulk(handle, validPtr, valid.byteLength, 1), 0);
  M._tx_free(validPtr);
  assert.equal(M._tx_bridge_heap_used(), 0);
  assert.ok(M._tx_native_heap_used() > nativeBaseline);

  assert.equal(M._terra_world_close(handle), 0);
  assert.equal(M._tx_bridge_heap_used(), 0);
  assert.equal(M._tx_native_heap_used(), 0);
});

