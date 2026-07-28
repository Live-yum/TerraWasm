"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const WLD_DIR = path.resolve(__dirname, "..", "..", "TerraX", "wld");
const WLD_FILES = fs.readdirSync(WLD_DIR).filter((name) => name.endsWith(".wld"));
const TEST_WLD = path.join(WLD_DIR, WLD_FILES.find((name) => name.includes("copy")) || WLD_FILES[0]);
const TEST_BYTES = fs.readFileSync(TEST_WLD);

function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function mustAlloc(M, size, label) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `${label}: tx_malloc(${size}) returned zero`);
  return ptr;
}

function readU32(M, ptr) {
  return M.HEAPU32[ptr >>> 2] >>> 0;
}

function writeCString(M, value, label) {
  const bytes = Buffer.from(`${value}\0`, "utf8");
  const ptr = mustAlloc(M, bytes.length, label);
  M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function executeOperation(M, handle, name, request) {
  const namePtr = writeCString(M, name, "operation name");
  const requestPtr = writeCString(M, JSON.stringify(request), "operation request");
  const requiredPtr = mustAlloc(M, 8, "operation required size");
  let responsePtr = 0;
  let response;
  try {
    let status = M._terra_op_execute_json(
      handle,
      namePtr,
      requestPtr,
      0,
      0n,
      requiredPtr,
    );
    assert.equal(status, 0, `${name} probe failed with status ${status}`);
    const required = readU32(M, requiredPtr);
    assert.ok(required > 1, `${name} must return a JSON response`);
    responsePtr = mustAlloc(M, required, "operation response");
    status = M._terra_op_execute_json(
      handle,
      namePtr,
      requestPtr,
      responsePtr,
      BigInt(required),
      requiredPtr,
    );
    assert.equal(status, 0, `${name} copy failed with status ${status}`);
    const responseBytes = M.HEAPU8.slice(responsePtr, responsePtr + required - 1);
    response = JSON.parse(Buffer.from(responseBytes).toString("utf8"));
  } finally {
    if (responsePtr) M._tx_free(responsePtr);
    M._tx_free(requiredPtr);
    M._tx_free(requestPtr);
    M._tx_free(namePtr);
  }
  return response;
}

function openBytes(M, bytes) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle output");
  let handle = 0;
  try {
    M.HEAPU8.set(bytes, inputPtr);
    const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
    assert.equal(status, 0, `open_from_buffer failed with status ${status}`);
    handle = readU32(M, handlePtr);
    assert.notEqual(handle, 0);
    return { handle, inputPtr, handlePtr };
  } catch (error) {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
    throw error;
  }
}

function releaseOpened(M, opened) {
  if (!opened) return;
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

test("buffer-native unmodified save is byte-stable and reopenable", async () => {
  const M = await TerraWorldWasm();
  assert.equal(
    typeof M._terra_world_save_to_buffer,
    "function",
    "terra_world_save_to_buffer export is required for the MEMFS-free save path",
  );

  let opened;
  let reopened;
  let requiredPtr = 0;
  let outputPtr = 0;
  try {
    opened = openBytes(M, TEST_BYTES);
    requiredPtr = mustAlloc(M, 4, "save required size");

    const heapBeforeProbe = M._tx_heap_used() >>> 0;
    let status = M._terra_world_save_to_buffer(opened.handle, 0, 0, requiredPtr);
    assert.equal(status, 0, `save buffer probe failed with status ${status}`);
    const required = readU32(M, requiredPtr);
    assert.equal(required, TEST_BYTES.length, "unmodified save size must match input size");
    assert.equal(
      M._tx_heap_used() >>> 0,
      heapBeforeProbe,
      "unmodified size probes must not allocate a temporary world output",
    );

    outputPtr = mustAlloc(M, required, "save output");
    status = M._terra_world_save_to_buffer(opened.handle, outputPtr, required - 1, requiredPtr);
    assert.equal(status, 2, "a short output buffer must be rejected");
    assert.equal(readU32(M, requiredPtr), required, "short-buffer failure must preserve required size");
    status = M._terra_world_save_to_buffer(opened.handle, outputPtr, required, requiredPtr);
    assert.equal(status, 0, `save buffer copy failed with status ${status}`);

    const output = Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
    assert.equal(sha256(output), sha256(TEST_BYTES), "unmodified save must preserve SHA-256");

    releaseOpened(M, opened);
    opened = null;
    reopened = openBytes(M, output);
    assert.notEqual(reopened.handle, 0, "saved bytes must reopen");
  } finally {
    releaseOpened(M, reopened);
    releaseOpened(M, opened);
    if (outputPtr) M._tx_free(outputPtr);
    if (requiredPtr) M._tx_free(requiredPtr);
  }
});

test("modified buffer save is reopenable and releases every tracked allocation", async () => {
  const M = await TerraWorldWasm();
  const heapBefore = M._tx_heap_used() >>> 0;
  let opened;
  let reopened;
  let requiredPtr = 0;
  let outputPtr = 0;
  try {
    opened = openBytes(M, TEST_BYTES);
    const mutation = executeOperation(M, opened.handle, "batch_update_tiles", {
      rules: [{ where: { is_active: true, tile_color: 0 }, patch: { tile_color: 5 }, limit: 1 }],
    });
    assert.ok(mutation.total_updated > 0, "the fixture must contain a tile changed by the mutation");

    requiredPtr = mustAlloc(M, 4, "modified save required size");
    let status = M._terra_world_save_to_buffer(opened.handle, 0, 0, requiredPtr);
    assert.equal(status, 0, `modified save probe failed with status ${status}`);
    const required = readU32(M, requiredPtr);
    assert.ok(required > 0, "modified save must report an output size");

    outputPtr = mustAlloc(M, required, "modified save output");
    status = M._terra_world_save_to_buffer(opened.handle, outputPtr, required, requiredPtr);
    assert.equal(status, 0, `modified save copy failed with status ${status}`);
    const output = Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
    assert.notEqual(sha256(output), sha256(TEST_BYTES), "a tile mutation must change the world bytes");

    releaseOpened(M, opened);
    opened = null;
    reopened = openBytes(M, output);
    assert.notEqual(reopened.handle, 0, "modified world bytes must reopen and parse");
  } finally {
    releaseOpened(M, reopened);
    releaseOpened(M, opened);
    if (outputPtr) M._tx_free(outputPtr);
    if (requiredPtr) M._tx_free(requiredPtr);
  }

  assert.equal(M._tx_native_heap_used() >>> 0, 0, "world close must release native allocations");
  assert.equal(M._tx_bridge_heap_used() >>> 0, 0, "caller allocations must be balanced");
  assert.equal(M._tx_heap_used() >>> 0, heapBefore, "tracked heap must return to baseline");
});

test("modified save serializes directly into caller memory without a native output copy", async () => {
  const M = await TerraWorldWasm();
  const heapBefore = M._tx_heap_used() >>> 0;
  let opened;
  let reopened;
  let requiredPtr = 0;
  let outputPtr = 0;
  try {
    opened = openBytes(M, TEST_BYTES);
    const mutation = executeOperation(M, opened.handle, "header_patch", {
      patch: { hardMode: true },
    });
    assert.equal(mutation.status, "ok");

    requiredPtr = mustAlloc(M, 4, "direct save required size");
    let status = M._terra_world_save_to_buffer(opened.handle, 0, 0, requiredPtr);
    assert.equal(status, 0, `direct save probe failed with status ${status}`);
    const required = readU32(M, requiredPtr);
    assert.ok(required > 0);

    outputPtr = mustAlloc(M, required, "direct save output");
    M.HEAPU8.fill(0xa5, outputPtr, outputPtr + required);
    const nativeBeforeSave = M._tx_native_heap_used() >>> 0;
    const nativePeakBeforeSave = M._tx_native_heap_peak() >>> 0;

    status = M._terra_world_save_to_buffer(
      opened.handle,
      outputPtr,
      required - 1,
      requiredPtr,
    );
    assert.equal(status, 2, "short direct output must be rejected before serialization");
    assert.equal(M.HEAPU8[outputPtr], 0xa5, "short-buffer failure must not touch caller bytes");
    assert.equal(M.HEAPU8[outputPtr + required - 1], 0xa5);
    assert.equal(M._tx_native_heap_used() >>> 0, nativeBeforeSave);

    status = M._terra_world_save_to_buffer(opened.handle, outputPtr, required, requiredPtr);
    assert.equal(status, 0, `direct modified save failed with status ${status}`);
    const nativeAfterSave = M._tx_native_heap_used() >>> 0;
    const nativePeakAfterSave = M._tx_native_heap_peak() >>> 0;
    console.log(JSON.stringify({
      fixtureBytes: TEST_BYTES.length,
      required,
      nativeBeforeSave,
      nativeAfterSave,
      nativePeakBeforeSave,
      nativePeakAfterSave,
    }));
    assert.equal(nativeAfterSave, nativeBeforeSave, "save must retain no native output bytes");
    assert.equal(
      nativePeakAfterSave,
      nativePeakBeforeSave,
      "save must not allocate a full native output before copying to caller memory",
    );

    const output = Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
    releaseOpened(M, opened);
    opened = null;
    reopened = openBytes(M, output);
    assert.notEqual(reopened.handle, 0, "directly serialized bytes must reopen");
  } finally {
    releaseOpened(M, reopened);
    releaseOpened(M, opened);
    if (outputPtr) M._tx_free(outputPtr);
    if (requiredPtr) M._tx_free(requiredPtr);
  }

  assert.equal(M._tx_native_heap_used() >>> 0, 0);
  assert.equal(M._tx_bridge_heap_used() >>> 0, 0);
  assert.equal(M._tx_heap_used() >>> 0, heapBefore);
});
