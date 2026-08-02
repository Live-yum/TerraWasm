"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TEST_WLD = getPrimaryWorldPath();

function alloc(M, bytes) {
  const ptr = M._tx_malloc(bytes.length || bytes);
  assert.notEqual(ptr, 0);
  if (bytes.length) M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function commandBuffer(json) {
  const payload = Buffer.from(json, "utf8");
  const output = Buffer.alloc(16 + 8 + payload.length);
  output.writeUInt32LE(0x31435854, 0); // TXC1
  output.writeUInt16LE(1, 4);
  output.writeUInt16LE(0, 6);
  output.writeUInt32LE(1, 8);
  output.writeUInt32LE(8 + payload.length, 12);
  output.writeUInt16LE(1, 16); // batch_update_tiles JSON command
  output.writeUInt16LE(0, 18);
  output.writeUInt32LE(payload.length, 20);
  payload.copy(output, 24);
  return output;
}

test("versioned command buffers apply tile operations and preserve malformed-input safety", async () => {
  const M = await TerraWorldWasm();
  const world = fs.readFileSync(TEST_WLD);
  const worldPtr = alloc(M, world);
  const handlePtr = alloc(M, 4);
  assert.equal(M._terra_world_open_from_buffer(worldPtr, world.length, handlePtr), 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;

  const command = commandBuffer(JSON.stringify({
    rules: [{ where: { type: 1 }, patch: { tile_color: 1 } }],
  }));
  const commandPtr = alloc(M, command);
  assert.equal(M._terra_world_apply_commands(handle, commandPtr, command.length), 0);

  const malformed = Buffer.from(command);
  malformed.writeUInt32LE(0xdeadbeef, 0);
  const malformedPtr = alloc(M, malformed);
  assert.equal(M._terra_world_apply_commands(handle, malformedPtr, malformed.length), 1);
  assert.equal(M._terra_world_apply_commands(handle, commandPtr, 15), 1);

  assert.equal(M._terra_world_close(handle), 0);
  assert.equal(M._terra_world_apply_commands(handle, commandPtr, command.length), 8);
  M._tx_free(malformedPtr);
  M._tx_free(commandPtr);
  M._tx_free(handlePtr);
  M._tx_free(worldPtr);
});
