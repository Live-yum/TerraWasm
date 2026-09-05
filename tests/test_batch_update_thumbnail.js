"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const TEST_WLD = getPrimaryWorldPath();
const TEST_BYTES = fs.readFileSync(TEST_WLD);
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const THUMB_MAX_WIDTH = 512;

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

function readU64(M, ptr) {
  return Number(new DataView(M.wasmMemory.buffer).getBigUint64(ptr, true));
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
  try {
    let status = M._terra_op_execute_json(handle, namePtr, requestPtr, 0, 0n, requiredPtr);
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
    return JSON.parse(Buffer.from(M.HEAPU8.slice(responsePtr, responsePtr + required - 1)).toString("utf8"));
  } finally {
    if (responsePtr) M._tx_free(responsePtr);
    M._tx_free(requiredPtr);
    M._tx_free(requestPtr);
    M._tx_free(namePtr);
  }
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
    assert.notEqual(handle, 0, "open_from_buffer returned a zero handle");
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

function renderThumbnailPng(M, handle, maxWidth, cacheKey) {
  const render = executeOperation(M, handle, "render_thumbnail_png", {
    max_w: maxWidth,
    cache_key: cacheKey,
  });
  assert.equal(render.status, "ok");

  const requiredPtr = mustAlloc(M, 8, "thumbnail required size");
  const widthPtr = mustAlloc(M, 4, "thumbnail width");
  const heightPtr = mustAlloc(M, 4, "thumbnail height");
  let outputPtr = 0;
  try {
    let status = M._terra_op_get_thumbnail_png(handle, 0, 0n, requiredPtr, widthPtr, heightPtr);
    assert.equal(status, 2, "thumbnail probe must report buffer-too-small");
    const required = readU64(M, requiredPtr);
    assert.ok(required > PNG_SIGNATURE.length, "thumbnail PNG must report a non-empty payload");
    outputPtr = mustAlloc(M, required, "thumbnail output");
    status = M._terra_op_get_thumbnail_png(handle, outputPtr, BigInt(required), requiredPtr, widthPtr, heightPtr);
    assert.equal(status, 0, `thumbnail copy failed with status ${status}`);

    const png = Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
    assert.deepEqual(png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE, "thumbnail must be a PNG");
    return {
      png,
      hash: sha256(png),
      width: readU32(M, widthPtr),
      height: readU32(M, heightPtr),
      reportedSize: required,
    };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(requiredPtr);
  }
}

function saveWorldToBuffer(M, handle) {
  const requiredPtr = mustAlloc(M, 4, "save required size");
  let outputPtr = 0;
  try {
    let status = M._terra_world_save_to_buffer(handle, 0, 0, requiredPtr);
    assert.equal(status, 0, `save probe failed with status ${status}`);
    const required = readU32(M, requiredPtr);
    assert.ok(required > 0, "save probe must report an output size");
    outputPtr = mustAlloc(M, required, "save output");
    status = M._terra_world_save_to_buffer(handle, outputPtr, required, requiredPtr);
    assert.equal(status, 0, `save copy failed with status ${status}`);
    return Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
  }
}

test("batch_update_tiles changes same-handle thumbnails and survives save/reopen", async () => {
  const M = await TerraWorldWasm();
  M._tx_reset_heap();
  const heapBefore = M._tx_heap_used() >>> 0;
  const inputHash = sha256(TEST_BYTES);

  let opened;
  let reopened;
  try {
    opened = openBytes(M, TEST_BYTES);
    const before = renderThumbnailPng(M, opened.handle, THUMB_MAX_WIDTH, "before");
    assert.ok(before.width > 0 && before.height > 0, "pre-mutation thumbnail dimensions must be populated");

    const mutation = executeOperation(M, opened.handle, "batch_update_tiles", {
      rules: [{ where: { is_active: true, type: 1 }, patch: { type: 25 } }],
    });
    assert.ok(mutation.total_updated > 0, "mutation must update at least one tile");

    const after = renderThumbnailPng(M, opened.handle, THUMB_MAX_WIDTH, "after");
    const savedBytes = saveWorldToBuffer(M, opened.handle);
    const savedHash = sha256(savedBytes);

    releaseOpened(M, opened);
    opened = null;

    reopened = openBytes(M, savedBytes);
    const reopenedThumb = renderThumbnailPng(M, reopened.handle, THUMB_MAX_WIDTH, "reopened");
    const repeatedMutation = executeOperation(M, reopened.handle, "batch_update_tiles", {
      rules: [{ where: { is_active: true, type: 25 }, patch: { type: 1 } }],
    });
    const diagnostics = {
      totalUpdated: mutation.total_updated,
      repeatedTotalUpdated: repeatedMutation.total_updated,
      beforeHash: before.hash,
      afterHash: after.hash,
      reopenedHash: reopenedThumb.hash,
      inputHash,
      savedHash,
    };

    assert.notEqual(
      after.hash,
      before.hash,
      `same-handle thumbnail must change after the mutation: ${JSON.stringify(diagnostics)}`,
    );
    assert.equal(after.width, before.width, "thumbnail width should remain stable for the same render request");
    assert.equal(after.height, before.height, "thumbnail height should remain stable for the same render request");
    assert.notEqual(
      savedHash,
      inputHash,
      `mutated world save must differ from the input bytes: ${JSON.stringify(diagnostics)}`,
    );
    assert.ok(
      repeatedMutation.total_updated > 0,
      `saved tile replacements must survive reopening: ${JSON.stringify(diagnostics)}`,
    );
    assert.equal(
      reopenedThumb.hash,
      after.hash,
      `reopened thumbnail must match the mutated same-handle thumbnail: ${JSON.stringify(diagnostics)}`,
    );
  } finally {
    releaseOpened(M, reopened);
    releaseOpened(M, opened);
  }

  assert.equal(M._tx_native_heap_used() >>> 0, 0, "world close must release native allocations");
  assert.equal(M._tx_bridge_heap_used() >>> 0, 0, "caller allocations must be balanced");
  assert.equal(M._tx_heap_used() >>> 0, heapBefore, "tracked heap must return to baseline");
});
