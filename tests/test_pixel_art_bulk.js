"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const WLD_PATH = getPrimaryWorldPath();
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

test("bulk indexed ABI rejects malformed records without leaking either heap", async () => {
  assert.ok(fs.existsSync(WLD_PATH), `missing checked-in world fixture: ${WLD_PATH}`);
  assert.ok(fs.existsSync(TXCI_PATH), `missing checked-in TXCI palette: ${TXCI_PATH}`);
  const M = await TerraWorldWasm();
  assert.equal(typeof M._txw_add_pixel_art_chunks_bulk, "function");
  assert.equal(typeof M._txw_add_pixel_art_chunks_bulk_fast, "function");
  assert.equal(typeof M._terra_world_commit_to_buffer, "function");

  function alloc(bytes) {
    const value = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const ptr = M._tx_malloc(value.byteLength);
    assert.notEqual(ptr, 0);
    M.HEAPU8.set(value, ptr);
    return ptr;
  }

  function beginIndexed(handle, txci, palette, width, height) {
    const txciPtr = alloc(txci);
    const palettePtr = alloc(palette);
    const status = M._txw_begin_pixel_art_indexed(
      handle,
      500, 500,
      width, height,
      palettePtr, 2,
      txciPtr, txci.byteLength,
      0, 0,
      0, 0,
      0,
    );
    M._tx_free(palettePtr);
    M._tx_free(txciPtr);
    assert.equal(status, 0);
  }

  const worldBytes = fs.readFileSync(WLD_PATH);
  const handlePtr = M._tx_malloc(4);
  const worldPtr = alloc(worldBytes);
  assert.equal(M._terra_world_open_from_buffer(worldPtr, worldBytes.byteLength, handlePtr), 0);
  let handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  M._tx_free(worldPtr);
  M._tx_free(handlePtr);

  const txci = fs.readFileSync(TXCI_PATH);
  const palette = Uint8Array.of(0, 0, 0, 0, 255, 0, 0, 255);
  beginIndexed(handle, txci, palette, 16, 16);

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

  beginIndexed(handle, txci, palette, 63 * 64, 1);
  const fastBaseline = M._tx_native_heap_used();

  const invalidFast = makeRecord({ reserved: 1 });
  const invalidFastPtr = alloc(invalidFast);
  assert.ok(M._txw_add_pixel_art_chunks_bulk_fast(
    handle,
    invalidFastPtr,
    invalidFast.byteLength,
    1,
  ) < 0);
  M._tx_free(invalidFastPtr);
  assert.equal(M._tx_native_heap_used(), fastBaseline);

  const fullBatch = new Uint8Array(RECORD_BYTES * 63);
  for (let index = 0; index < 63; index += 1) {
    fullBatch.set(makeRecord({ cx: index }), index * RECORD_BYTES);
  }
  const fullBatchPtr = alloc(fullBatch);
  assert.equal(M._txw_add_pixel_art_chunks_bulk_fast(
    handle,
    fullBatchPtr,
    fullBatch.byteLength,
    63,
  ), 0);
  M._tx_free(fullBatchPtr);
  assert.equal(M._tx_bridge_heap_used(), 0);
  assert.ok(M._tx_native_heap_used() > fastBaseline);

  const requiredPtr = M._tx_malloc(4);
  const newHandlePtr = M._tx_malloc(4);
  assert.notEqual(requiredPtr, 0);
  assert.notEqual(newHandlePtr, 0);
  assert.equal(M._terra_world_commit_to_buffer(
    handle,
    0,
    0,
    requiredPtr,
    newHandlePtr,
  ), 0);
  const required = M.HEAPU32[requiredPtr >>> 2] >>> 0;
  assert.ok(required > 16);
  assert.equal(M.HEAPU32[newHandlePtr >>> 2] >>> 0, 0);

  const outputPtr = M._tx_malloc(required);
  assert.notEqual(outputPtr, 0);
  assert.equal(M._terra_world_commit_to_buffer(
    handle,
    outputPtr,
    required,
    requiredPtr,
    newHandlePtr,
  ), 0);
  handle = M.HEAPU32[newHandlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  assert.equal(M.HEAPU32[requiredPtr >>> 2] >>> 0, required);
  assert.equal(M.HEAPU8[outputPtr], worldBytes[0]);

  M._tx_free(outputPtr);
  M._tx_free(newHandlePtr);
  M._tx_free(requiredPtr);
  assert.equal(M._tx_bridge_heap_used(), 0);

  assert.equal(M._terra_world_close(handle), 0);
  assert.equal(M._tx_bridge_heap_used(), 0);
  assert.equal(M._tx_native_heap_used(), 0);
});

test("direct pixel-art ABI validates bridge ranges and retains only addressed RGBA bytes", async () => {
  const M = await TerraWorldWasm();
  const worldBytes = fs.readFileSync(WLD_PATH);
  const txci = fs.readFileSync(TXCI_PATH);

  function alloc(value) {
    const bytes = typeof value === "number" ? new Uint8Array(value) : new Uint8Array(value);
    const ptr = M._tx_malloc(bytes.byteLength || 1);
    assert.notEqual(ptr, 0);
    if (bytes.byteLength) M.HEAPU8.set(bytes, ptr);
    return ptr;
  }

  const inputPtr = alloc(worldBytes);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  assert.equal(M._terra_world_open_from_buffer(inputPtr, worldBytes.byteLength, handlePtr), 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  M._tx_free(inputPtr);
  M._tx_free(handlePtr);

  try {
    const mapBytes = new Uint8Array(12);
    mapBytes[10] = 1;
    const mapPtr = alloc(mapBytes);
    const tinyPixelsPtr = alloc(Uint8Array.of(1, 2, 3, 255));
    const nativeBeforeShortQueue = M._tx_native_heap_used();
    assert.ok(M._txw_queue_pixel_art(
      handle, 0, 0, 2, 2,
      tinyPixelsPtr, 16,
      mapPtr, 1, 1,
    ) < 0, "short bridge allocation must be rejected before memcpy");
    assert.equal(M._tx_native_heap_used(), nativeBeforeShortQueue);
    M._tx_free(tinyPixelsPtr);

    const oversized = new Uint8Array(1024 * 1024);
    oversized.set([1, 2, 3, 255]);
    const oversizedPtr = alloc(oversized);
    const nativeBeforeOversizedQueue = M._tx_native_heap_used();
    assert.equal(M._txw_queue_pixel_art(
      handle, 0, 0, 1, 1,
      oversizedPtr, oversized.byteLength,
      mapPtr, 1, 1,
    ), 0);
    const retainedQueueBytes = M._tx_native_heap_used() - nativeBeforeOversizedQueue;
    assert.ok(
      retainedQueueBytes < 1024,
      `1x1 queue retained ${retainedQueueBytes} bytes instead of only its addressed RGBA/maps prefix`,
    );
    M._tx_free(oversizedPtr);
    M._tx_free(mapPtr);

    const txciPtr = alloc(txci);
    const palettePtr = alloc(Uint8Array.of(0, 0, 0, 0, 255, 0, 0, 255));
    const tinyPalettePtr = alloc(Uint8Array.of(0, 0, 0, 0));
    const nativeBeforeBadBegin = M._tx_native_heap_used();
    assert.ok(M._txw_begin_pixel_art_indexed(
      handle, 0, 0, 64, 64,
      tinyPalettePtr, 2,
      txciPtr, txci.byteLength,
      0, 0, 0, 0, 0,
    ) < 0, "palette count may not exceed the bridge allocation");
    assert.equal(M._tx_native_heap_used(), nativeBeforeBadBegin);
    M._tx_free(tinyPalettePtr);

    assert.equal(M._txw_begin_pixel_art_indexed(
      handle, 0, 0, 64, 64,
      palettePtr, 2,
      txciPtr, txci.byteLength,
      0, 0, 0, 0, 0,
    ), 0);
    const tinyIndicesPtr = alloc(new Uint8Array(2));
    const nativeBeforeBadChunk = M._tx_native_heap_used();
    assert.ok(M._txw_add_pixel_art_chunk(
      handle, 0, 0, tinyIndicesPtr, 4096, 1,
    ) < 0, "chunk count may not outgrow the bridge allocation");
    assert.equal(M._tx_native_heap_used(), nativeBeforeBadChunk);
    M._tx_free(tinyIndicesPtr);

    const tinyImagePtr = alloc(Uint8Array.of(1, 2, 3, 255));
    const nativeBeforeBadApply = M._tx_native_heap_used();
    assert.ok(M._txw_apply_pixel_art(
      handle,
      tinyImagePtr, 16, 2, 2,
      txciPtr, txci.byteLength,
      0, 0, 0, 0, 0, 0,
    ) < 0, "integrated pixel-art must reject a claimed image length beyond its bridge allocation");
    assert.equal(M._tx_native_heap_used(), nativeBeforeBadApply);
    M._tx_free(tinyImagePtr);

    M._tx_free(palettePtr);
    M._tx_free(txciPtr);
    assert.equal(M._tx_bridge_heap_used(), 0);
  } finally {
    assert.equal(M._terra_world_close(handle), 0);
    assert.equal(M._tx_bridge_heap_used(), 0);
    assert.equal(M._tx_native_heap_used(), 0);
  }
});

test("generic operation output paths cannot escape the working directory", async () => {
  const M = await TerraWorldWasm();
  const worldBytes = fs.readFileSync(WLD_PATH);

  function allocBytes(bytes) {
    const value = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const ptr = M._tx_malloc(value.byteLength);
    assert.notEqual(ptr, 0);
    M.HEAPU8.set(value, ptr);
    return ptr;
  }

  function allocCString(value) {
    const size = M.lengthBytesUTF8(value) + 1;
    const ptr = M._tx_malloc(size);
    assert.notEqual(ptr, 0);
    M.stringToUTF8(value, ptr, size);
    return ptr;
  }

  function execute(handle, name, request) {
    const namePtr = allocCString(name);
    const requestPtr = allocCString(JSON.stringify(request));
    const requiredPtr = M._tx_malloc(8);
    assert.notEqual(requiredPtr, 0);
    try {
      return M._terra_op_execute_json(handle, namePtr, requestPtr, 0, 0n, requiredPtr);
    } finally {
      M._tx_free(requiredPtr);
      M._tx_free(requestPtr);
      M._tx_free(namePtr);
    }
  }

  const inputPtr = allocBytes(worldBytes);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  assert.equal(M._terra_world_open_from_buffer(
    inputPtr,
    worldBytes.byteLength,
    handlePtr,
  ), 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);
  M._tx_free(inputPtr);
  M._tx_free(handlePtr);

  try {
    assert.notEqual(execute(handle, "render_preview_png", {
      output_path: "../escaped.png",
      max_w: 8,
    }), 0);
    assert.notEqual(execute(handle, "render_preview_png", {
      output_path: path.resolve("escaped.png"),
      max_w: 8,
    }), 0);
    assert.notEqual(execute(handle, "render_preview_png", {
      output_path: "tests/not-a-png.txt",
      max_w: 8,
    }), 0);
    assert.notEqual(execute(handle, "render_lit_map", {
      output_dir: "../escaped-map",
    }), 0);

    const missingDirectory = path.join("tests", "missing-operation-output-dir");
    fs.rmSync(missingDirectory, { recursive: true, force: true });
    assert.notEqual(execute(handle, "render_preview_png", {
      output_path: path.join(missingDirectory, "preview.png"),
      max_w: 8,
    }), 0, "a failed PNG write must not be reported as success");
    assert.equal(fs.existsSync(path.join(missingDirectory, "preview.png")), false);
  } finally {
    assert.equal(M._terra_world_close(handle), 0);
  }
});
