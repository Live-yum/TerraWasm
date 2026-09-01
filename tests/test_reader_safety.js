"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");
const path = require("node:path");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

function alloc(M, bytes) {
  const ptr = M._tx_malloc(bytes.length || 1);
  assert.notEqual(ptr, 0);
  if (bytes.length) M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function readLastError(M) {
  const requiredPtr = M._tx_malloc(4);
  assert.notEqual(requiredPtr, 0);
  let bufferPtr = 0;
  try {
    assert.equal(M._terra_get_last_error_json(0, 0, requiredPtr), 0);
    const required = M.HEAPU32[requiredPtr >>> 2] >>> 0;
    assert(required > 1);
    bufferPtr = M._tx_malloc(required);
    assert.notEqual(bufferPtr, 0);
    assert.equal(M._terra_get_last_error_json(bufferPtr, required, requiredPtr), 0);
    return JSON.parse(M.UTF8ToString(bufferPtr, required));
  } finally {
    if (bufferPtr) M._tx_free(bufferPtr);
    M._tx_free(requiredPtr);
  }
}

function makeOverlongHeaderString() {
  const bytes = Buffer.alloc(96);
  let offset = 0;
  bytes.writeUInt32LE(135, offset); offset += 4;
  Buffer.from("relogic", "ascii").copy(bytes, offset); offset += 7;
  bytes[offset++] = 2;
  bytes.writeUInt32LE(0, offset); offset += 4;
  bytes.writeBigUInt64LE(0n, offset); offset += 8;
  bytes.writeUInt16LE(1, offset); offset += 2;
  bytes.writeUInt32LE(32, offset); offset += 4;
  bytes.writeUInt16LE(0, offset); offset += 2;
  assert.equal(offset, 32);
  bytes[offset++] = 0x7f;
  return bytes.subarray(0, offset);
}

function makeCrossSectionHeaderString() {
  const bytes = Buffer.alloc(96);
  let offset = 0;
  bytes.writeUInt32LE(135, offset); offset += 4;
  Buffer.from("relogic", "ascii").copy(bytes, offset); offset += 7;
  bytes[offset++] = 2;
  bytes.writeUInt32LE(0, offset); offset += 4;
  bytes.writeBigUInt64LE(0n, offset); offset += 8;
  bytes.writeUInt16LE(2, offset); offset += 2;
  bytes.writeUInt32LE(36, offset); offset += 4;
  bytes.writeUInt32LE(40, offset); offset += 4;
  bytes.writeUInt16LE(0, offset); offset += 2;
  assert.equal(offset, 36);
  bytes[offset++] = 5;
  bytes[offset++] = 0x41;
  bytes[offset++] = 0x42;
  bytes[offset++] = 0x43;
  bytes[offset++] = 0x44;
  bytes[offset++] = 0x45;
  return bytes.subarray(0, offset);
}

function makeTruncatedFixedHeader() {
  const bytes = Buffer.alloc(96);
  let offset = 0;
  bytes.writeUInt32LE(135, offset); offset += 4;
  Buffer.from("relogic", "ascii").copy(bytes, offset); offset += 7;
  bytes[offset++] = 2;
  bytes.writeUInt32LE(0, offset); offset += 4;
  bytes.writeBigUInt64LE(0n, offset); offset += 8;
  bytes.writeUInt16LE(2, offset); offset += 2;
  bytes.writeUInt32LE(36, offset); offset += 4;
  bytes.writeUInt32LE(40, offset); offset += 4;
  bytes.writeUInt16LE(0, offset); offset += 2;
  assert.equal(offset, 36);
  bytes[offset++] = 0;
  return bytes.subarray(0, offset);
}

test("truncated and corrupted world buffers fail as statuses without crashing", async () => {
  const M = await TerraWorldWasm();
  for (const candidate of [
    Buffer.alloc(16),
    Buffer.from([88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
  ]) {
    const inputPtr = alloc(M, candidate);
    const handlePtr = M._tx_malloc(4);
    assert.notEqual(handlePtr, 0);
    try {
      const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
      assert.notEqual(status, 0);
      assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
    } finally {
      M._tx_free(handlePtr);
      M._tx_free(inputPtr);
    }
  }
});

test("an overlong header string is rejected without reading beyond the world buffer", async () => {
  const M = await TerraWorldWasm();
  const candidate = makeOverlongHeaderString();
  const inputPtr = alloc(M, candidate);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  try {
    const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
    assert.notEqual(status, 0);
    assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
  } finally {
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
  }
});

test("a header string cannot cross into the next WLD section", async () => {
  const M = await TerraWorldWasm();
  const candidate = makeCrossSectionHeaderString();
  const inputPtr = alloc(M, candidate);
  const handlePtr = M._tx_malloc(4);
  assert.notEqual(handlePtr, 0);
  try {
    const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
    assert.equal(status, 5);
    assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
    assert.deepEqual(readLastError(M), {
      code: "TERRAX_TRUNCATED_HEADER",
      message: "string exceeds header section bounds",
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


test("WLD current-version guard matches Terraria release 326", async () => {
  const M = await TerraWorldWasm();

  for (const [version, expectedCode] of [
    [326, "TERRAX_TRUNCATED_FORMAT"],
    [327, "TERRAX_UNSUPPORTED_VERSION"],
  ]) {
    // The public open ABI rejects buffers shorter than 16 bytes before the
    // WLD parser runs. Keep the candidate at that minimum so release 326
    // reaches parse_format() and fails on missing metadata, while release 327
    // is rejected by the version guard before metadata is consumed.
    const candidate = Buffer.alloc(16);
    candidate.writeUInt32LE(version, 0);
    const inputPtr = alloc(M, candidate);
    const handlePtr = M._tx_malloc(4);
    assert.notEqual(handlePtr, 0);
    try {
      const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
      assert.notEqual(status, 0);
      assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
      assert.equal(readLastError(M).code, expectedCode,
        `unexpected WLD version guard for release ${version}`);
    } finally {
      M._tx_free(handlePtr);
      M._tx_free(inputPtr);
    }
  }
});