"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath, getWorldFixturePaths } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const WORLD_FIXTURE_PATHS = getWorldFixturePaths();
const TEST_WLD = getPrimaryWorldPath();
const TEST_BYTES = fs.readFileSync(TEST_WLD);
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

let modulePromise;
function loadModule() {
  modulePromise ||= TerraWorldWasm();
  return modulePromise;
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

function readRequiredSize(M, ptr) {
  return readU64(M, ptr);
}

function readLastErrorJson(M) {
  const sizePtr = mustAlloc(M, 8, "error required size");
  let outputPtr = 0;
  try {
    let status = M._terra_info_get_last_error_json(0, 0n, sizePtr);
    assert.equal(status, 0, "last-error probe must succeed");
    const required = readRequiredSize(M, sizePtr);
    if (!required) return {};
    outputPtr = mustAlloc(M, required, "error output");
    status = M._terra_info_get_last_error_json(outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0, "last-error copy must succeed");
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
  }
}

function operationFailureMessage(operationName, status, error) {
  return `${operationName} failed with status=${status}, error=${JSON.stringify(error)}`;
}

function openBuffer(M, bytes = TEST_BYTES) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle output");
  let handle = 0;
  try {
    M.HEAPU8.set(bytes, inputPtr);
    const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
    assert.equal(status, 0, `terra_world_open_from_buffer failed with status ${status}`);
    handle = readU32(M, handlePtr);
    assert.notEqual(handle, 0, "terra_world_open_from_buffer returned a zero handle");
    return { handle, inputPtr, handlePtr };
  } catch (error) {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
    throw error;
  }
}

function releaseOpenBuffer(M, opened, close = true) {
  if (!opened) return;
  if (close && opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

function allocCString(M, value) {
  const length = M.lengthBytesUTF8(value) + 1;
  const ptr = mustAlloc(M, length, `string ${value}`);
  M.stringToUTF8(value, ptr, length);
  return ptr;
}

function readSectionJson(M, handle, sectionName) {
  const namePtr = allocCString(M, sectionName);
  const sizePtr = mustAlloc(M, 8, `${sectionName} size`);
  let outputPtr = 0;
  try {
    let status = M._terra_section_get_json(handle, namePtr, 0, 0n, sizePtr);
    assert.equal(status, 0, `${sectionName} size query failed with status ${status}`);
    const required = readRequiredSize(M, sizePtr);
    assert.ok(required > 1, `${sectionName} size query returned ${required}`);
    outputPtr = mustAlloc(M, required, `${sectionName} output`);
    status = M._terra_section_get_json(handle, namePtr, outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0, `${sectionName} copy failed with status ${status}`);
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
    M._tx_free(namePtr);
  }
}

function probeOperation(M, handle, operationName, request = "{}") {
  const operationPtr = allocCString(M, operationName);
  const requestPtr = allocCString(M, request);
  const sizePtr = mustAlloc(M, 8, `${operationName} size`);
  try {
    const status = M._terra_op_execute_json(
      handle,
      operationPtr,
      requestPtr,
      0,
      0n,
      sizePtr,
    );
    return { status, required: readRequiredSize(M, sizePtr), operationPtr, requestPtr, sizePtr };
  } catch (error) {
    M._tx_free(sizePtr);
    M._tx_free(requestPtr);
    M._tx_free(operationPtr);
    throw error;
  }
}

function releaseProbe(M, probe) {
  if (!probe) return;
  M._tx_free(probe.sizePtr);
  M._tx_free(probe.requestPtr);
  M._tx_free(probe.operationPtr);
}

function executeOperationJson(M, handle, operationName, request) {
  const operationPtr = allocCString(M, operationName);
  const requestPtr = allocCString(M, JSON.stringify(request));
  const sizePtr = mustAlloc(M, 8, `${operationName} size`);
  let outputPtr = 0;
  try {
    let status = M._terra_op_execute_json(handle, operationPtr, requestPtr, 0, 0n, sizePtr);
    if (status !== 0) return { status, value: null };
    const required = readRequiredSize(M, sizePtr);
    outputPtr = mustAlloc(M, required, `${operationName} output`);
    status = M._terra_op_execute_json(
      handle,
      operationPtr,
      requestPtr,
      outputPtr,
      BigInt(required),
      sizePtr,
    );
    return { status, value: status === 0 ? M.UTF8ToString(outputPtr) : null };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
    M._tx_free(requestPtr);
    M._tx_free(operationPtr);
  }
}

function firstStoredChest(chests) {
  for (const chest of chests) {
    if ((chest.items || []).some((item) => item && item.stack > 0)) return chest;
  }
  assert.fail("fixture must contain at least one chest with a stored item");
}

function firstStoredItem(chests) {
  return (firstStoredChest(chests).items || []).find((item) => item && item.stack > 0).itemType;
}

function sha256Hex(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function getThumbnailPng(M, handle) {
  const requiredPtr = mustAlloc(M, 8, "thumbnail required size");
  const widthPtr = mustAlloc(M, 4, "thumbnail width");
  const heightPtr = mustAlloc(M, 4, "thumbnail height");
  let outputPtr = 0;
  try {
    let status = M._terra_op_get_thumbnail_png(handle, 0, 0n, requiredPtr, widthPtr, heightPtr);
    assert.equal(status, 2, "thumbnail probe must report buffer-too-small");
    const required = readRequiredSize(M, requiredPtr);
    assert.ok(required > PNG_SIGNATURE.length, "thumbnail PNG must report a non-empty payload");
    outputPtr = mustAlloc(M, required, "thumbnail output");
    status = M._terra_op_get_thumbnail_png(
      handle,
      outputPtr,
      BigInt(required),
      requiredPtr,
      widthPtr,
      heightPtr,
    );
    assert.equal(status, 0, `thumbnail copy failed with status ${status}`);
    return {
      png: Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required)),
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

function getMarkedMap(M, handle) {
  const requiredPtr = mustAlloc(M, 8, "map required size");
  const widthPtr = mustAlloc(M, 4, "map width");
  const heightPtr = mustAlloc(M, 4, "map height");
  let outputPtr = 0;
  try {
    let status = M._terra_op_get_map(handle, 0, 0n, requiredPtr, widthPtr, heightPtr);
    assert.equal(status, 2, "map probe must report buffer-too-small");
    const required = readRequiredSize(M, requiredPtr);
    assert.ok(required > 2, "marked map must report a non-empty payload");
    outputPtr = mustAlloc(M, required, "map output");
    status = M._terra_op_get_map(handle, outputPtr, BigInt(required), requiredPtr, widthPtr, heightPtr);
    assert.equal(status, 0, `map copy failed with status ${status}`);
    return {
      map: Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required)),
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

function inspectFixtureHeader(M, filePath) {
  const bytes = fs.readFileSync(filePath);
  let opened;
  try {
    opened = openBuffer(M, bytes);
    const header = readSectionJson(M, opened.handle, "header");
    return { filePath, bytes, header };
  } finally {
    releaseOpenBuffer(M, opened);
  }
}

function find8400x2400Fixture(M) {
  const sortedFiles = [...WORLD_FIXTURE_PATHS].sort((left, right) => left.localeCompare(right));
  for (const filePath of sortedFiles) {
    const fixture = inspectFixtureHeader(M, filePath);
    if (fixture.header.maxTilesX === 8400 && fixture.header.maxTilesY === 2400) return fixture;
  }
  assert.fail("missing required 8400x2400 fixture in checked-in tests/*.wld");
}

function findFirstStoredItemInBytes(M, bytes) {
  let opened;
  try {
    opened = openBuffer(M, bytes);
    return firstStoredItem(readSectionJson(M, opened.handle, "chests"));
  } finally {
    releaseOpenBuffer(M, opened);
  }
}

function findUniqueStoredChest(chests) {
  const itemCounts = new Map();
  for (const chest of chests) {
    for (const item of chest.items || []) {
      if (item && item.stack > 0) {
        itemCounts.set(item.itemType, (itemCounts.get(item.itemType) || 0) + 1);
      }
    }
  }
  for (let chestIndex = 0; chestIndex < chests.length; chestIndex++) {
    const chest = chests[chestIndex];
    for (const item of chest.items || []) {
      if (item && item.stack > 0 && itemCounts.get(item.itemType) === 1) {
        return { chestIndex, itemType: item.itemType };
      }
    }
  }
  return null;
}

function findAbsentStoredChestItem(chests) {
  const used = new Set();
  for (const chest of chests) {
    for (const item of chest.items || []) {
      if (item && item.stack > 0) used.add(item.itemType);
    }
  }
  for (let candidate = 1000000; candidate >= 1; candidate -= 1) {
    if (!used.has(candidate)) return candidate;
  }
  return 0;
}

function read7BitEncodedLength(buffer, state) {
  let value = 0;
  let shift = 0;
  while (true) {
    const byte = buffer[state.offset++];
    value |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return value >>> 0;
    shift += 7;
  }
}

function skipChestRecord(buffer, state, version, slotsPerChest) {
  state.offset += 8;
  const nameLen = read7BitEncodedLength(buffer, state);
  state.offset += nameLen;
  const maxItems = version >= 294 ? buffer.readInt32LE(state.offset) : slotsPerChest;
  if (version >= 294) state.offset += 4;
  const itemSlots = maxItems > 0 ? maxItems : 0;
  for (let i = 0; i < itemSlots; i += 1) {
    const stack = buffer.readUInt16LE(state.offset);
    state.offset += 2;
    if (stack !== 0) state.offset += 5;
  }
}

function computeChestPatchOffset(M, handle, bytes, chestIndex) {
  const header = readSectionJson(M, handle, "header");
  const format = readSectionJson(M, handle, "format");
  const state = { offset: format.positions[2] };
  const chestCount = bytes.readUInt16LE(state.offset);
  state.offset += 2;
  const slotsPerChest = header.version < 294 ? bytes.readUInt16LE(state.offset) : 0;
  if (header.version < 294) state.offset += 2;
  assert.ok(chestIndex >= 0 && chestIndex < chestCount, "target chest index must exist");
  for (let i = 0; i < chestIndex; i += 1) {
    skipChestRecord(bytes, state, header.version, slotsPerChest);
  }
  return { offset: state.offset, width: header.maxTilesX >>> 0, height: header.maxTilesY >>> 0 };
}

test("tx_free returns bridge allocation usage to baseline", async () => {
  const M = await loadModule();
  M._tx_reset_heap();
  const baseline = M._tx_heap_used() >>> 0;
  const ptr = mustAlloc(M, 4096, "bridge allocation");
  assert.ok((M._tx_heap_used() >>> 0) >= baseline + 4096);
  M._tx_free(ptr);
  assert.equal(M._tx_heap_used() >>> 0, baseline, "tx_free must release bridge-owned payload bytes");
  M._tx_free(ptr);
  M._tx_free(ptr + 1);
  assert.equal(M._tx_heap_used() >>> 0, baseline, "duplicate or interior frees must be ignored");
});

test("100 open/read/close cycles keep live heap within 64 KiB of baseline", async () => {
  const M = await loadModule();
  let warmup;
  try {
    warmup = openBuffer(M);
    readSectionJson(M, warmup.handle, "format");
  } finally {
    releaseOpenBuffer(M, warmup);
  }
  M._tx_reset_heap();
  const baselineHeap = M._tx_heap_used() >>> 0;
  const baselineLinear = M.wasmMemory.buffer.byteLength;
  let completed = 0;
  for (let index = 0; index < 100; index += 1) {
    let opened;
    try {
      opened = openBuffer(M);
      const format = readSectionJson(M, opened.handle, "format");
      assert.ok(format && typeof format === "object");
    } finally {
      releaseOpenBuffer(M, opened);
    }
    completed += 1;
    const currentHeap = M._tx_heap_used() >>> 0;
    if (currentHeap - baselineHeap > 64 * 1024) {
      assert.fail(`live heap exceeded 64 KiB after ${completed} cycles: fixture=${TEST_BYTES.length}, baseline=${baselineHeap}, current=${currentHeap}, linear=${M.wasmMemory.buffer.byteLength}, baselineLinear=${baselineLinear}`);
    }
  }
  const finalHeap = M._tx_heap_used() >>> 0;
  assert.ok(finalHeap - baselineHeap <= 64 * 1024);
});

test("a stale handle cannot close a newly opened generation", async () => {
  const M = await loadModule();
  M._tx_reset_heap();
  let first;
  let second;
  try {
    first = openBuffer(M);
    const staleHandle = first.handle;
    assert.equal(M._terra_world_close(staleHandle), 0);
    releaseOpenBuffer(M, first, false);
    first = null;
    second = openBuffer(M);
    const staleCloseStatus = M._terra_world_close(staleHandle);
    assert.notEqual(staleCloseStatus, 0, "closing a stale generation must be rejected");
    assert.ok(readSectionJson(M, second.handle, "format"));
  } finally {
    releaseOpenBuffer(M, first);
    releaseOpenBuffer(M, second);
  }
});

test("oversized allocation fails without returning address zero as writable memory", async () => {
  const M = await loadModule();
  const ptr = M._tx_malloc(0xffffffff);
  assert.equal(ptr, 0, "overflowing allocation must return zero");
});

test("operation probes are exact-keyed and media survives bridge allocations", async () => {
  const M = await loadModule();
  let opened;
  let probe;
  let unrelatedPtr = 0;
  let mediaPtr = 0;
  const sizePtr = mustAlloc(M, 8, "media size");
  const widthPtr = mustAlloc(M, 4, "media width");
  const heightPtr = mustAlloc(M, 4, "media height");
  try {
    opened = openBuffer(M);
    probe = probeOperation(M, opened.handle, "render_thumbnail_png", "{\"max_w\":64}");
    assert.equal(probe.status, 0);
    const heapAfterFirstProbe = M._tx_heap_used() >>> 0;
    const repeatedStatus = M._terra_op_execute_json(opened.handle, probe.operationPtr, probe.requestPtr, 0, 0n, probe.sizePtr);
    assert.equal(repeatedStatus, 0);
    assert.equal(M._tx_heap_used() >>> 0, heapAfterFirstProbe, "same probe must not re-execute");

    unrelatedPtr = mustAlloc(M, 4096, "unrelated bridge allocation");
    let status = M._terra_op_get_thumbnail_png(opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 2);
    const required = readRequiredSize(M, sizePtr);
    assert.ok(required > PNG_SIGNATURE.length);
    mediaPtr = mustAlloc(M, required, "media output");
    status = M._terra_op_get_thumbnail_png(opened.handle, mediaPtr, BigInt(required), sizePtr, widthPtr, heightPtr);
    assert.equal(status, 0);
    assert.deepEqual(Buffer.from(M.HEAPU8.slice(mediaPtr, mediaPtr + PNG_SIGNATURE.length)), PNG_SIGNATURE);

    const wrongNamePtr = allocCString(M, "nonexistent_op");
    const responsePtr = mustAlloc(M, probe.required || 64, "mismatched response");
    try {
      status = M._terra_op_execute_json(opened.handle, wrongNamePtr, probe.requestPtr, responsePtr, BigInt(probe.required || 64), probe.sizePtr);
      assert.notEqual(status, 0, "a mismatched copy call must not receive the cached response");
    } finally {
      M._tx_free(responsePtr);
      M._tx_free(wrongNamePtr);
    }
  } finally {
    if (mediaPtr) M._tx_free(mediaPtr);
    if (unrelatedPtr) M._tx_free(unrelatedPtr);
    releaseProbe(M, probe);
    releaseOpenBuffer(M, opened);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
});

test("new failures and close invalidate world-owned media without stale reads", async () => {
  const M = await loadModule();
  let opened;
  let renderProbe;
  let failureProbe;
  const sizePtr = mustAlloc(M, 8, "media size");
  const widthPtr = mustAlloc(M, 4, "media width");
  const heightPtr = mustAlloc(M, 4, "media height");
  try {
    opened = openBuffer(M);
    renderProbe = probeOperation(M, opened.handle, "render_thumbnail_png", "{\"max_w\":32}");
    assert.equal(renderProbe.status, 0);
    failureProbe = probeOperation(M, opened.handle, "nonexistent_op", "{}");
    assert.notEqual(failureProbe.status, 0);
    let status = M._terra_op_get_thumbnail_png(opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 8, "a failed replacement operation must invalidate prior media");

    releaseProbe(M, renderProbe);
    renderProbe = probeOperation(M, opened.handle, "render_thumbnail_png", "{\"max_w\":32}");
    assert.equal(renderProbe.status, 0);
    const staleHandle = opened.handle;
    assert.equal(M._terra_world_close(staleHandle), 0);
    releaseOpenBuffer(M, opened, false);
    opened = null;
    status = M._terra_op_get_thumbnail_png(staleHandle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 8, "close-between-probe-and-copy must reject the stale handle");
  } finally {
    releaseProbe(M, failureProbe);
    releaseProbe(M, renderProbe);
    releaseOpenBuffer(M, opened);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
});

test("section probes are single-use, exact-indexed, and bridge-allocation safe", async () => {
  const M = await loadModule();
  let opened;
  const namePtr = allocCString(M, "header");
  const unknownPtr = allocCString(M, "unknown-section");
  const sizePtr = mustAlloc(M, 8, "section size");
  let outputPtr = 0;
  try {
    opened = openBuffer(M);
    let status = M._terra_section_get_json(opened.handle, namePtr, 0, 0n, sizePtr);
    assert.equal(status, 0);
    const required = readU32(M, sizePtr);
    assert.ok(required > 1);
    const heapAfterProbe = M._tx_heap_used() >>> 0;
    status = M._terra_section_get_json(opened.handle, namePtr, 0, 0n, sizePtr);
    assert.equal(status, 0);
    assert.equal(M._tx_heap_used() >>> 0, heapAfterProbe, "repeated section probes must reuse the result");
    outputPtr = mustAlloc(M, required, "section output");
    status = M._terra_section_get_json(opened.handle, namePtr, outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0);
    assert.ok(JSON.parse(M.UTF8ToString(outputPtr)).maxTilesX > 0);
    status = M._terra_section_get_json(opened.handle, namePtr, 0, 0n, sizePtr);
    assert.equal(status, 0);
    const heapWithCachedHeader = M._tx_heap_used() >>> 0;
    status = M._terra_section_get_json(opened.handle, unknownPtr, 0, 0n, sizePtr);
    assert.equal(status, 3);
    assert.ok((M._tx_heap_used() >>> 0) < heapWithCachedHeader, "a failed replacement read must release the prior cached section");
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    releaseOpenBuffer(M, opened);
    M._tx_free(sizePtr);
    M._tx_free(unknownPtr);
    M._tx_free(namePtr);
  }
});

test("schema probes do not leak and oversized operation inputs are rejected", async () => {
  const M = await loadModule();
  assert.equal(typeof M._tx_native_heap_used, "function");
  assert.equal(typeof M._tx_bridge_heap_used, "function");
  assert.equal(typeof M._tx_heap_peak, "function");
  const schemaNamePtr = allocCString(M, "header");
  const sizePtr = mustAlloc(M, 8, "schema size");
  let opened;
  let longNamePtr = 0;
  let longRequestPtr = 0;
  try {
    const nativeBaseline = M._tx_native_heap_used() >>> 0;
    for (let index = 0; index < 3; index += 1) {
      assert.equal(M._terra_info_get_section_schema_json(schemaNamePtr, 0, 0n, sizePtr), 0);
      assert.ok(readU32(M, sizePtr) > 1);
      assert.equal(M._tx_native_heap_used() >>> 0, nativeBaseline, "schema probes must free their temporary serializer buffer");
    }
    opened = openBuffer(M);
    longNamePtr = allocCString(M, "x".repeat(64));
    M.HEAPU32[sizePtr >>> 2] = 0xdeadbeef;
    let status = M._terra_op_execute_json(opened.handle, longNamePtr, schemaNamePtr, 0, 0n, sizePtr);
    assert.equal(status, 1);
    assert.equal(readU32(M, sizePtr), 0, "rejected operation names must clear required size");
    longRequestPtr = allocCString(M, "x".repeat(1024 * 1024 + 1));
    status = M._terra_op_execute_json(opened.handle, schemaNamePtr, longRequestPtr, 0, 0n, sizePtr);
    assert.equal(status, 1);
    assert.equal(readU32(M, sizePtr), 0, "rejected operation requests must clear required size");
  } finally {
    if (longRequestPtr) M._tx_free(longRequestPtr);
    if (longNamePtr) M._tx_free(longNamePtr);
    releaseOpenBuffer(M, opened);
    M._tx_free(sizePtr);
    M._tx_free(schemaNamePtr);
  }
});

test("marker map then preview png can fail a later invalid request without leaking native heap after world close", async () => {
  const M = await loadModule();
  M._tx_reset_heap();
  const baselineNative = M._tx_native_heap_used() >>> 0;
  const nativeTolerance = 64 * 1024;
  let opened;
  let capturedError = null;
  try {
    opened = openBuffer(M);
    const header = readSectionJson(M, opened.handle, "header");
    const chests = readSectionJson(M, opened.handle, "chests");
    const itemType = firstStoredItem(chests);
    let result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: [{ item_id: itemType, color: "#FF3300FF" }],
    });
    try {
      assert.equal(result.status, 0);
      const mapResponse = JSON.parse(result.value);
      assert.equal(mapResponse.status, "ok");
      assert.ok(mapResponse.map_bytes > 0);
      result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_preview", {
        chest_markers: [{ item_id: itemType, color: "#FF3300FF" }], max_w: 512, max_h: 256,
      });
      if (result.status !== 0) {
        const error = readLastErrorJson(M);
        assert.equal(result.status, 0, operationFailureMessage("mark_tiles_and_chests_preview", result.status, error));
      }
      const previewResponse = JSON.parse(result.value);
      assert.equal(previewResponse.status, "ok");
      assert.ok(previewResponse.thumbnail_png_bytes > PNG_SIGNATURE.length);
      assert.ok(previewResponse.matched_chest_count > 0);
      assert.ok(previewResponse.width > 0);
      assert.ok(previewResponse.height > 0);
      const thumbnail = getThumbnailPng(M, opened.handle);
      assert.equal(thumbnail.reportedSize, previewResponse.thumbnail_png_bytes);
      assert.ok(thumbnail.width > 0 && thumbnail.height > 0);
      assert.equal(thumbnail.width, previewResponse.width);
      assert.equal(thumbnail.height, previewResponse.height);
      assert.deepEqual(thumbnail.png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE);
      assert.ok(thumbnail.width <= header.maxTilesX && thumbnail.height <= header.maxTilesY, "preview PNG must stay within the source world dimensions");
      result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_map", {});
      assert.notEqual(result.status, 0, "invalid marker request must fail after prior successful media operations");
    } catch (error) {
      capturedError = error;
    }
  } finally {
    releaseOpenBuffer(M, opened);
  }
  const nativeAfterClose = M._tx_native_heap_used() >>> 0;
  assert.ok(nativeAfterClose <= baselineNative + nativeTolerance, `native heap must return near baseline after close: baseline=${baselineNative}, afterClose=${nativeAfterClose}, tolerance=${nativeTolerance}`);
  if (capturedError) throw capturedError;
});

test("real fixture marker map and preview can run 10 cycles without native heap growth", async (t) => {
  const inspectModule = await TerraWorldWasm();
  const fixture = find8400x2400Fixture(inspectModule);
  const fixtureName = path.basename(fixture.filePath);
  const fixtureDims = `${fixture.header.maxTilesX}x${fixture.header.maxTilesY}`;
  const fixtureItemType = findFirstStoredItemInBytes(inspectModule, fixture.bytes);
  const fixtureSha = sha256Hex(fixture.bytes);
  const cycles = 10;
  const nativeTolerance = 64 * 1024;
  const M = await loadModule();
  let warmup;
  try {
    warmup = openBuffer(M, fixture.bytes);
    readSectionJson(M, warmup.handle, "header");
  } finally {
    releaseOpenBuffer(M, warmup);
  }
  M._tx_reset_heap();
  const baselineNative = M._tx_native_heap_used() >>> 0;
  const cycleDiagnostics = [];
  let maxAfterClose = baselineNative;
  for (let cycle = 1; cycle <= cycles; cycle += 1) {
    let opened;
    try {
      assert.equal(sha256Hex(fixture.bytes), fixtureSha, `fixture bytes must stay unchanged before cycle ${cycle}`);
      opened = openBuffer(M, fixture.bytes);
      let result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_map", {
        chest_markers: [{ item_id: fixtureItemType, color: "#FF3300FF" }],
      });
      if (result.status !== 0) {
        const error = readLastErrorJson(M);
        assert.equal(result.status, 0, operationFailureMessage("mark_tiles_and_chests_map", result.status, error));
      }
      const mapResponse = JSON.parse(result.value);
      assert.equal(mapResponse.status, "ok");
      assert.ok(mapResponse.map_bytes > 0);
      assert.ok(mapResponse.matched_chest_count > 0);
      assert.equal(mapResponse.width, fixture.header.maxTilesX);
      assert.equal(mapResponse.height, fixture.header.maxTilesY);
      const markedMap = getMarkedMap(M, opened.handle);
      assert.equal(markedMap.reportedSize, mapResponse.map_bytes);
      assert.equal(markedMap.width, mapResponse.width);
      assert.equal(markedMap.height, mapResponse.height);
      assert.equal(markedMap.map.readUInt16LE(0), 33083);
      result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_preview", {
        chest_markers: [{ item_id: fixtureItemType, color: "#FF3300FF" }], max_w: 512, max_h: 256,
      });
      if (result.status !== 0) {
        const error = readLastErrorJson(M);
        assert.equal(result.status, 0, operationFailureMessage("mark_tiles_and_chests_preview", result.status, error));
      }
      const previewResponse = JSON.parse(result.value);
      assert.equal(previewResponse.status, "ok");
      assert.ok(previewResponse.thumbnail_png_bytes > PNG_SIGNATURE.length);
      assert.equal(previewResponse.matched_chest_count, mapResponse.matched_chest_count);
      assert.ok(previewResponse.width > 0);
      assert.ok(previewResponse.height > 0);
      assert.ok(previewResponse.width <= fixture.header.maxTilesX && previewResponse.height <= fixture.header.maxTilesY);
      const thumbnail = getThumbnailPng(M, opened.handle);
      assert.equal(thumbnail.reportedSize, previewResponse.thumbnail_png_bytes);
      assert.equal(thumbnail.width, previewResponse.width);
      assert.equal(thumbnail.height, previewResponse.height);
      assert.deepEqual(thumbnail.png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE);
      cycleDiagnostics.push({ cycle, mapBytes: mapResponse.map_bytes, pngBytes: previewResponse.thumbnail_png_bytes, liveBeforeClose: M._tx_native_heap_used() >>> 0, peakDuringCycle: M._tx_native_heap_peak() >>> 0 });
    } finally {
      releaseOpenBuffer(M, opened);
    }
    const afterClose = M._tx_native_heap_used() >>> 0;
    maxAfterClose = Math.max(maxAfterClose, afterClose);
    cycleDiagnostics[cycleDiagnostics.length - 1].afterClose = afterClose;
    assert.ok(afterClose <= baselineNative + nativeTolerance, `native heap must return near baseline after cycle ${cycle} for ${fixtureName}: baseline=${baselineNative}, afterClose=${afterClose}, tolerance=${nativeTolerance}`);
    assert.equal(sha256Hex(fixture.bytes), fixtureSha, `fixture bytes must stay unchanged after cycle ${cycle}`);
  }
  const finalNative = M._tx_native_heap_used() >>> 0;
  assert.ok(finalNative <= baselineNative + nativeTolerance);
  assert.ok(maxAfterClose <= baselineNative + nativeTolerance);
  assert.ok(cycleDiagnostics[cycleDiagnostics.length - 1].peakDuringCycle <= cycleDiagnostics[0].peakDuringCycle + nativeTolerance);
  const peakNative = cycleDiagnostics.reduce((max, entry) => Math.max(max, entry.peakDuringCycle), baselineNative);
  t.diagnostic(JSON.stringify({ fixture: fixtureName, dimensions: fixtureDims, cycles, nativeBaseline: baselineNative, nativeFinal: finalNative, nativePeak: peakNative, nativeMaxAfterClose: maxAfterClose, tolerance: nativeTolerance }));
});

test("8400x2400 marker map keeps intermediate native peak below 16 MiB beyond input and final output", async () => {
  const inspectModule = await TerraWorldWasm();
  const fixture = find8400x2400Fixture(inspectModule);
  const fixtureName = path.basename(fixture.filePath);
  const fixtureDims = `${fixture.header.maxTilesX}x${fixture.header.maxTilesY}`;
  const fixtureItemType = findFirstStoredItemInBytes(inspectModule, fixture.bytes);
  const M = await TerraWorldWasm();
  M._tx_reset_heap();
  const baselineNative = M._tx_native_heap_used() >>> 0;
  let opened;
  const sizePtr = mustAlloc(M, 8, "map required size");
  const widthPtr = mustAlloc(M, 4, "map width");
  const heightPtr = mustAlloc(M, 4, "map height");
  let mapOutputPtr = 0;
  try {
    opened = openBuffer(M, fixture.bytes);
    const setupLive = M._tx_native_heap_used() >>> 0;
    const peakBeforeMap = M._tx_native_heap_peak() >>> 0;
    assert.equal(peakBeforeMap, setupLive, "after reset and a clean open, setup peak must equal setup live before map generation");
    const result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: [{ item_id: fixtureItemType, color: "#FF2020C8" }],
    });
    assert.equal(result.status, 0);
    const response = JSON.parse(result.value);
    assert.equal(response.status, "ok");
    assert.ok(response.map_bytes > 0);
    assert.equal(response.width, fixture.header.maxTilesX);
    assert.equal(response.height, fixture.header.maxTilesY);
    const nativeAfterMap = M._tx_native_heap_used() >>> 0;
    const peakDuringMap = M._tx_native_heap_peak() >>> 0;
    const finalMapLive = nativeAfterMap - setupLive;
    const extraPeakNative = peakDuringMap - setupLive - finalMapLive;
    let getterStatus = M._terra_op_get_map(opened.handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(getterStatus, 2, "map probe must report buffer-too-small");
    const getterRequiredBytes = readRequiredSize(M, sizePtr);
    const getterWidth = readU32(M, widthPtr);
    const getterHeight = readU32(M, heightPtr);
    assert.equal(getterRequiredBytes, response.map_bytes);
    assert.equal(getterWidth, response.width);
    assert.equal(getterHeight, response.height);
    mapOutputPtr = mustAlloc(M, getterRequiredBytes, "map output");
    const getterCapacityBytes = getterRequiredBytes;
    getterStatus = M._terra_op_get_map(opened.handle, mapOutputPtr, BigInt(getterCapacityBytes), sizePtr, widthPtr, heightPtr);
    assert.equal(getterStatus, 0, "map copy must succeed with the probed capacity");
    const getterActualBytes = readRequiredSize(M, sizePtr);
    const diagnostics = { fixture: fixtureName, dimensions: fixtureDims, baselineNative, setupLive, finalMapLive, peakDuringMap, extraPeakNative, mapBytes: response.map_bytes, getterRequiredBytes, getterCapacityBytes, getterActualBytes, matchedChests: response.matched_chest_count };
    assert.ok(extraPeakNative < 16 * 1024 * 1024, `marker map intermediate native peak exceeded 16 MiB for ${fixtureName} (${fixtureDims}): ${JSON.stringify(diagnostics)}`);
  } finally {
    if (mapOutputPtr) M._tx_free(mapOutputPtr);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
    releaseOpenBuffer(M, opened);
  }
});

test("marked map ignores chests that fall outside the world bounds", async () => {
  const M = await TerraWorldWasm();
  let opened;
  try {
    opened = openBuffer(M, find8400x2400Fixture(M).bytes);
    const originalChests = readSectionJson(M, opened.handle, "chests");
    const chestItemType = findAbsentStoredChestItem(originalChests);
    assert.ok(chestItemType, "fixture must contain at least one absent chest item type");
    const chestRequest = { chests: [{ x: 0, y: 0, name: "", maxItems: 1, items: [{ stack: 1, itemType: chestItemType, prefix: 0 }] }] };
    const update = executeOperationJson(M, opened.handle, "replace_chests", chestRequest);
    assert.equal(update.status, 0);
    const firstResult = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: [{ item_id: chestItemType, color: "#FF2020C8" }],
    });
    assert.equal(firstResult.status, 0);
    const firstResponse = JSON.parse(firstResult.value);
    assert.equal(firstResponse.status, "ok");
    assert.ok(firstResponse.map_bytes > 0);
    assert.equal(firstResponse.matched_chest_count, 1);
    const header = readSectionJson(M, opened.handle, "header");
    const memory = Buffer.from(M.wasmMemory.buffer);
    const pattern = Buffer.alloc(22);
    let offset = 0;
    pattern.writeUInt16LE(1, offset); offset += 2;
    pattern.writeUInt32LE(0, offset); offset += 4;
    pattern.writeUInt32LE(0, offset); offset += 4;
    pattern.writeUInt8(0, offset); offset += 1;
    pattern.writeUInt32LE(1, offset); offset += 4;
    pattern.writeUInt16LE(1, offset); offset += 2;
    pattern.writeUInt32LE(chestItemType, offset); offset += 4;
    pattern.writeUInt8(0, offset);
    const chestOffset = memory.indexOf(pattern);
    assert.ok(chestOffset >= 0, "replace_chests override bytes must be present in WASM memory");
    const view = new DataView(M.wasmMemory.buffer);
    view.setUint32(chestOffset + 2, header.maxTilesX >>> 0, true);
    view.setUint32(chestOffset + 6, header.maxTilesY >>> 0, true);
    const patchedChest = readSectionJson(M, opened.handle, "chests")[0];
    assert.equal(patchedChest.x, header.maxTilesX >>> 0);
    assert.equal(patchedChest.y, header.maxTilesY >>> 0);
    const result = executeOperationJson(M, opened.handle, "mark_tiles_and_chests_map", {
      chest_markers: [{ item_id: chestItemType, color: "#FF2020C8" }],
    });
    assert.equal(result.status, 0);
    const response = JSON.parse(result.value);
    assert.equal(response.status, "ok");
    assert.ok(response.map_bytes > 0);
    assert.equal(response.matched_chest_count, 0, "out-of-world chest coordinates must not be counted after boundary filtering");
  } finally {
    releaseOpenBuffer(M, opened);
  }
});
