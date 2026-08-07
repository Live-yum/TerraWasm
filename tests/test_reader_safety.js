"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const {
  CORRUPTED_FIXTURE_DIR,
  TRUNCATED_FIXTURE_DIR,
  readFixtureDirectory,
} = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

function alloc(M, bytes) {
  const ptr = M._tx_malloc(bytes.length);
  assert.notEqual(ptr, 0);
  M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function readLastError(M) {
  const requiredPtr = M._tx_malloc(8);
  assert.notEqual(requiredPtr, 0);
  let outputPtr = 0;
  try {
    assert.equal(M._terra_info_get_last_error_json(0, 0n, requiredPtr), 0);
    const required = Number(new DataView(M.wasmMemory.buffer).getBigUint64(requiredPtr, true));
    outputPtr = M._tx_malloc(required);
    assert.notEqual(outputPtr, 0);
    assert.equal(
      M._terra_info_get_last_error_json(outputPtr, BigInt(required), requiredPtr),
      0,
    );
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
  }
}

function makeCrossSectionHeader() {
  const candidate = Buffer.alloc(32);
  candidate.writeUInt32LE(88, 0);
  candidate.writeUInt16LE(2, 4);
  candidate.writeUInt32LE(16, 6);
  candidate.writeUInt32LE(18, 10);
  candidate.writeUInt16LE(0, 14);
  candidate[16] = 2;
  candidate[17] = 0x41;
  candidate[18] = 0x42;
  return candidate;
}

function makeTruncatedFixedHeader() {
  // Format section: version 88, two section pointers and zero important tiles.
  // Header section contains a valid world name and all seven fixed int32 values
  // through maxTilesY/maxTilesX, then ends before the remaining fixed header.
  const headerStart = 16;
  const headerEnd = headerStart + 2 + 7 * 4;
  const candidate = Buffer.alloc(headerEnd + 1);
  candidate.writeUInt32LE(88, 0);
  candidate.writeUInt16LE(2, 4);
  candidate.writeUInt32LE(headerStart, 6);
  candidate.writeUInt32LE(headerEnd, 10);
  candidate.writeUInt16LE(0, 14);

  let offset = headerStart;
  candidate[offset++] = 1;
  candidate[offset++] = 0x41;
  candidate.writeInt32LE(1, offset); offset += 4; // worldId
  candidate.writeInt32LE(0, offset); offset += 4; // left
  candidate.writeInt32LE(8400, offset); offset += 4; // right
  candidate.writeInt32LE(0, offset); offset += 4; // top
  candidate.writeInt32LE(2400, offset); offset += 4; // bottom
  candidate.writeInt32LE(2400, offset); offset += 4; // maxTilesY
  candidate.writeInt32LE(8400, offset); offset += 4; // maxTilesX
  assert.equal(offset, headerEnd);
  return candidate;
}

test("truncated and corrupted world buffers fail as statuses without crashing", async () => {
  const M = await TerraWorldWasm();
  const candidates = [
    ...readFixtureDirectory(TRUNCATED_FIXTURE_DIR).map((fixturePath) => fs.readFileSync(fixturePath)),
    ...readFixtureDirectory(CORRUPTED_FIXTURE_DIR).map((fixturePath) => fs.readFileSync(fixturePath)),
    Uint8Array.from({ length: 4096 }, (_, index) => (index * 73) & 0xff),
    crypto.randomBytes(4096),
  ];
  for (const candidate of candidates) {
    const inputPtr = alloc(M, candidate);
    const task = M._terra_world_open_begin(inputPtr, candidate.length);
    assert.notEqual(task, 0);
    const status = M._terra_world_open_step(task, 1);
    assert.notEqual(status, 0, `unexpected success for ${candidate.length}-byte invalid input`);
    assert.ok(status >= 1 && status <= 11);
    assert.equal(M._terra_world_task_get_world_handle(task), 0);
    assert.equal(M._terra_world_task_close(task), 0);
    M._tx_free(inputPtr);
  }
});

test("an overlong header string is rejected without reading beyond the world buffer", async () => {
  const M = await TerraWorldWasm();
  const candidate = Buffer.alloc(16);
  candidate.writeUInt32LE(88, 0);
  candidate.writeUInt16LE(1, 4);
  candidate.writeUInt32LE(12, 6);
  candidate.writeUInt16LE(0, 10);
  candidate[12] = 0x7f;

  const inputPtr = alloc(M, candidate);
  const task = M._terra_world_open_begin(inputPtr, candidate.length);
  assert.notEqual(task, 0);
  const status = M._terra_world_open_step(task, 1);
  assert.notEqual(status, 0);
  assert.equal(M._terra_world_task_get_world_handle(task), 0);
  assert.equal(M._terra_world_task_close(task), 0);
  M._tx_free(inputPtr);
});

test("a header string cannot cross into the next WLD section", async () => {
  const M = await TerraWorldWasm();
  const candidate = makeCrossSectionHeader();
  const inputPtr = alloc(M, candidate);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  try {
    const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
    assert.equal(status, 5);
    assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
    assert.deepEqual(readLastError(M), {
      code: "TERRAX_TRUNCATED_HEADER",
      message: "world name exceeds section bounds",
    });
  } finally {
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
  }
});

test("a fixed-width WLD header cannot be zero-filled after truncation", async () => {
  const M = await TerraWorldWasm();
  const candidate = makeTruncatedFixedHeader();
  const inputPtr = alloc(M, candidate);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  try {
    const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
    assert.equal(status, 5);
    assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
    assert.deepEqual(readLastError(M), {
      code: "TERRAX_TRUNCATED_HEADER",
      message: "fixed header fields exceed section bounds",
    });
  } finally {
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
  }
});
