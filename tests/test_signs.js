"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TEST_WLD = path.join(__dirname, "pixel_art_output.wld");

function encode7Bit(value) {
  const bytes = [];
  let remaining = value >>> 0;
  while (remaining >= 0x80) {
    bytes.push((remaining & 0x7f) | 0x80);
    remaining >>>= 7;
  }
  bytes.push(remaining);
  return Buffer.from(bytes);
}

function buildSignSection(text, x, y) {
  const textBytes = Buffer.from(text, "utf8");
  const coordinates = Buffer.alloc(8);
  coordinates.writeInt32LE(x, 0);
  coordinates.writeInt32LE(y, 4);
  return Buffer.concat([
    Buffer.from([1, 0]),
    encode7Bit(textBytes.length),
    textBytes,
    coordinates,
  ]);
}

function withSignSection(world, signSection) {
  const version = world.readUInt32LE(0);
  const pointerBase = version >= 135 ? 24 : 4;
  const pointerCount = world.readUInt16LE(pointerBase);
  const pointerOffset = pointerBase + 2;
  const positions = [];
  for (let index = 0; index < pointerCount; index += 1) {
    positions.push(world.readUInt32LE(pointerOffset + index * 4));
  }

  const signStart = positions[3];
  const signEnd = positions[4] ?? world.length;
  const delta = signSection.length - (signEnd - signStart);
  const output = Buffer.concat([
    world.subarray(0, signStart),
    signSection,
    world.subarray(signEnd),
  ]);

  for (let index = 4; index < pointerCount; index += 1) {
    output.writeUInt32LE(positions[index] + delta, pointerOffset + index * 4);
  }
  return output;
}

function alloc(M, bytes, label) {
  const ptr = M._tx_malloc(bytes.length || bytes);
  assert.notEqual(ptr, 0, `${label} allocation failed`);
  if (bytes.length) M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function readU64(M, ptr) {
  return Number(new DataView(M.wasmMemory.buffer).getBigUint64(ptr, true));
}

function readSectionJson(M, handle, sectionName) {
  const name = Buffer.from(`${sectionName}\0`, "utf8");
  const namePtr = alloc(M, name, "section name");
  const requiredPtr = alloc(M, 8, "section size");
  let outputPtr = 0;
  try {
    let status = M._terra_section_get_json(handle, namePtr, 0, 0n, requiredPtr);
    assert.equal(status, 0, "sign section size probe failed");
    const required = readU64(M, requiredPtr);
    outputPtr = alloc(M, required, "section output");
    status = M._terra_section_get_json(
      handle,
      namePtr,
      outputPtr,
      BigInt(required),
      requiredPtr,
    );
    assert.equal(status, 0, "sign section read failed");
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
    M._tx_free(namePtr);
  }
}

test("opens and serializes Terraria sign records as text, x, y", async () => {
  const M = await TerraWorldWasm();
  const text = "sign-order-中文-".repeat(12);
  const world = withSignSection(
    fs.readFileSync(TEST_WLD),
    buildSignSection(text, 4474, 575),
  );
  const inputPtr = alloc(M, world, "world input");
  const handlePtr = alloc(M, 4, "world handle");
  let handle = 0;
  try {
    const status = M._terra_world_open_from_buffer(inputPtr, world.length, handlePtr);
    assert.equal(status, 0, "world with a non-empty sign section must open");
    handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
    assert.notEqual(handle, 0);
    assert.deepEqual(readSectionJson(M, handle, "signs"), [{ x: 4474, y: 575, text }]);
  } finally {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
  }
});
