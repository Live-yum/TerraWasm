"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const WORLD_BYTES = fs.readFileSync(getPrimaryWorldPath());
const TXCI_GZ = fs.readFileSync(path.join(__dirname, "..", "data", "terraria_color_index.txci.gz"));

function mustAlloc(M, bytes, label) {
  const size = typeof bytes === "number" ? bytes : bytes.byteLength;
  const ptr = M._tx_malloc(Math.max(1, size));
  assert.notEqual(ptr, 0, `${label}: allocation failed`);
  if (typeof bytes !== "number" && bytes.byteLength) M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function openWorld(M) {
  const inputPtr = mustAlloc(M, WORLD_BYTES, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle");
  const status = M._terra_world_open_from_buffer(inputPtr, WORLD_BYTES.length, handlePtr);
  assert.equal(status, 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  return { handle, inputPtr, handlePtr };
}

function closeWorld(M, opened) {
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

test("marker icon atlas validates full bridge ranges and accepts valid interior tables", async () => {
  const M = await TerraWorldWasm();
  const opened = openWorld(M);
  const iconSize = 4;
  const rgbaBytes = new Uint8Array(iconSize * iconSize * 4).fill(255);
  const rgbaPtr = mustAlloc(M, rgbaBytes, "valid RGBA");
  const tinyRgbaPtr = mustAlloc(M, Uint8Array.of(1, 2, 3, 4), "short RGBA");
  const tablesPtr = mustAlloc(M, 12, "combined icon tables");
  const idsPtr = tablesPtr;
  const xOffsetsPtr = tablesPtr + 4;
  const yOffsetsPtr = tablesPtr + 8;
  const tableView = new DataView(M.HEAPU8.buffer);
  tableView.setUint32(idsPtr, 49, true);
  tableView.setUint32(xOffsetsPtr, 0, true);
  tableView.setUint32(yOffsetsPtr, 0, true);

  try {
    const nativeBaseline = Number(M._tx_native_heap_used());

    assert.ok(M._txw_set_icon_atlas(
      opened.handle,
      tinyRgbaPtr,
      iconSize,
      1,
      iconSize,
      iconSize,
      idsPtr,
      xOffsetsPtr,
      yOffsetsPtr,
    ) < 0, "short RGBA bridge allocation must be rejected before reads");
    assert.equal(Number(M._tx_native_heap_used()), nativeBaseline);

    const tinyIdsPtr = mustAlloc(M, 2, "short ID table");
    try {
      assert.ok(M._txw_set_icon_atlas(
        opened.handle,
        rgbaPtr,
        iconSize,
        1,
        iconSize,
        iconSize,
        tinyIdsPtr,
        xOffsetsPtr,
        yOffsetsPtr,
      ) < 0, "short item ID bridge allocation must be rejected before reads");
      assert.equal(Number(M._tx_native_heap_used()), nativeBaseline);
    } finally {
      M._tx_free(tinyIdsPtr);
    }

    // x/y pointers deliberately point inside the same 12-byte bridge root.
    // The validator must accept legal interior subranges used by packed callers.
    assert.equal(M._txw_set_icon_atlas(
      opened.handle,
      rgbaPtr,
      iconSize,
      1,
      iconSize,
      iconSize,
      idsPtr,
      xOffsetsPtr,
      yOffsetsPtr,
    ), 1);
    assert.ok(Number(M._tx_native_heap_used()) > nativeBaseline);

    assert.equal(M._txw_clear_icon_atlas(opened.handle), 0);
    assert.equal(Number(M._tx_native_heap_used()), nativeBaseline);
  } finally {
    M._tx_free(tablesPtr);
    M._tx_free(tinyRgbaPtr);
    M._tx_free(rgbaPtr);
    closeWorld(M, opened);
    assert.equal(Number(M._tx_bridge_heap_used()), 0);
    assert.equal(Number(M._tx_native_heap_used()), 0);
  }
});

test("marker color-index wrapper rejects a claimed gzip length outside its bridge allocation", async () => {
  const M = await TerraWorldWasm();
  const opened = openWorld(M);
  const tinyTxciPtr = mustAlloc(M, Uint8Array.of(0x1f, 0x8b, 0x08, 0x00), "short TXCI gzip");
  try {
    const nativeBaseline = Number(M._tx_native_heap_used());
    assert.ok(
      M._txw_set_marker_color_index(opened.handle, tinyTxciPtr, TXCI_GZ.length) < 0,
      "claimed TXCI length may not exceed the live bridge allocation",
    );
    assert.equal(Number(M._tx_native_heap_used()), nativeBaseline);
  } finally {
    M._tx_free(tinyTxciPtr);
    closeWorld(M, opened);
    assert.equal(Number(M._tx_bridge_heap_used()), 0);
    assert.equal(Number(M._tx_native_heap_used()), 0);
  }
});
