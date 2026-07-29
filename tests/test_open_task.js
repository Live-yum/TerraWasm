"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TEST_WLD = getPrimaryWorldPath();

function alloc(M, bytes, label) {
  const ptr = M._tx_malloc(bytes.length || bytes);
  assert.notEqual(ptr, 0, `${label} allocation failed`);
  if (bytes.length) M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function readU64(M, ptr) {
  return Number(M.HEAPU32[ptr >>> 2] >>> 0) + Number(M.HEAPU32[(ptr >>> 2) + 1] >>> 0) * 0x100000000;
}

test("open task reports progress, validates the first thumbnail, and transfers a world handle", async () => {
  const M = await TerraWorldWasm();
  const input = fs.readFileSync(TEST_WLD);
  const inputPtr = alloc(M, input, "world input");
  const task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  assert.equal(M._terra_world_task_get_progress(task), 0);

  let status = M._terra_world_open_step(task, 1);
  assert.equal(status, 10, "the first step remains in progress while rendering the thumbnail");
  assert.ok(M._terra_world_task_get_progress(task) > 0);
  status = M._terra_world_open_step(task, 1);
  assert.equal(status, 0);
  assert.equal(M._terra_world_task_get_progress(task), 100);

  const handlePtr = alloc(M, 4, "world handle");
  status = M._terra_world_open_finish(task, handlePtr);
  assert.equal(status, 0);
  const handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
  assert.notEqual(handle, 0);

  const requiredPtr = alloc(M, 8, "thumbnail size");
  const widthPtr = alloc(M, 4, "thumbnail width");
  const heightPtr = alloc(M, 4, "thumbnail height");
  status = M._terra_op_get_thumbnail_png(handle, 0, 0n, requiredPtr, widthPtr, heightPtr);
  assert.equal(status, 2);
  const required = readU64(M, requiredPtr);
  assert.ok(required > 8);
  const outputPtr = alloc(M, required, "thumbnail output");
  status = M._terra_op_get_thumbnail_png(handle, outputPtr, BigInt(required), requiredPtr, widthPtr, heightPtr);
  assert.equal(status, 0);
  assert.equal(M.HEAPU8[outputPtr], 0x89);
  assert.equal(M.HEAPU8[outputPtr + 1], 0x50);

  M._terra_world_close(handle);
  M._terra_world_task_close(task);
  M._tx_free(outputPtr);
  M._tx_free(heightPtr);
  M._tx_free(widthPtr);
  M._tx_free(requiredPtr);
  M._tx_free(handlePtr);
  M._tx_free(inputPtr);
});

test("canceling a task before work preserves the native heap baseline", async () => {
  const M = await TerraWorldWasm();
  const input = fs.readFileSync(TEST_WLD);
  const inputPtr = alloc(M, input, "world input");
  const baseline = M._tx_native_heap_used() >>> 0;
  const task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  assert.equal(M._terra_world_open_cancel(task), 11);
  assert.equal(M._terra_world_task_get_status(task), 11);
  assert.equal(M._terra_world_open_finish(task, 0), 8);
  M._terra_world_task_close(task);
  M._tx_free(inputPtr);
  assert.equal(M._tx_native_heap_used() >>> 0, baseline);
});
