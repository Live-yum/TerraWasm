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

function assertMonotonic(values, label) {
  for (let index = 1; index < values.length; index += 1) {
    assert.ok(values[index] >= values[index - 1], `${label} regressed at ${index}: ${values[index - 1]} -> ${values[index]}`);
  }
}

function runToCompletion(M, task, budget = 1, maxSteps = 10000) {
  const progress = [M._terra_world_task_get_progress(task) >>> 0];
  let status = 10;
  let steps = 0;
  while (status === 10 && steps < maxSteps) {
    status = M._terra_world_open_step(task, budget);
    progress.push(M._terra_world_task_get_progress(task) >>> 0);
    steps += 1;
  }
  assert.notEqual(steps, maxSteps, "incremental open exceeded the step safety limit");
  return { status, steps, progress };
}

test("open task consumes work units incrementally, reports real progress, and transfers a world handle", async () => {
  const M = await TerraWorldWasm();
  const input = fs.readFileSync(TEST_WLD);
  const inputPtr = alloc(M, input, "world input");
  const task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  assert.equal(M._terra_world_task_get_progress(task), 0);

  assert.equal(M._terra_world_open_step(task, 0), 10, "zero work units must be a no-op");
  assert.equal(M._terra_world_task_get_progress(task), 0, "zero-budget step changed progress");

  const firstStatus = M._terra_world_open_step(task, 1);
  const firstProgress = M._terra_world_task_get_progress(task) >>> 0;
  assert.equal(firstStatus, 10);
  assert.ok(firstProgress > 0 && firstProgress < 20, `first bounded step jumped to ${firstProgress}%`);

  const completed = runToCompletion(M, task, 1);
  assert.equal(completed.status, 0);
  assert.ok(completed.steps > 4, `work_units=1 completed too coarsely in ${completed.steps} follow-up steps`);
  assertMonotonic([firstProgress, ...completed.progress], "open progress");
  assert.ok(new Set([firstProgress, ...completed.progress]).size >= 5, "progress did not expose enough incremental stages");
  assert.equal(M._terra_world_task_get_progress(task), 100);

  const handlePtr = alloc(M, 4, "world handle");
  let status = M._terra_world_open_finish(task, handlePtr);
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
  assert.equal(M.HEAPU32[widthPtr >>> 2] >>> 0, 384, "initial thumbnail width changed");

  M._terra_world_close(handle);
  M._terra_world_task_close(task);
  M._tx_free(outputPtr);
  M._tx_free(heightPtr);
  M._tx_free(widthPtr);
  M._tx_free(requiredPtr);
  M._tx_free(handlePtr);
  M._tx_free(inputPtr);
});

test("larger work_units budget advances farther without changing task semantics", async () => {
  const M = await TerraWorldWasm();
  const input = fs.readFileSync(TEST_WLD);
  const inputPtr = alloc(M, input, "world input");
  const baseline = M._tx_native_heap_used() >>> 0;

  let task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  assert.equal(M._terra_world_open_step(task, 1), 10);
  const oneUnitProgress = M._terra_world_task_get_progress(task) >>> 0;
  assert.equal(M._terra_world_open_cancel(task), 11);
  assert.equal(M._terra_world_task_close(task), 0);
  assert.equal(M._tx_native_heap_used() >>> 0, baseline);

  task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  assert.equal(M._terra_world_open_step(task, 4), 10);
  const fourUnitProgress = M._terra_world_task_get_progress(task) >>> 0;
  assert.ok(fourUnitProgress > oneUnitProgress,
    `four work units (${fourUnitProgress}%) did not outpace one (${oneUnitProgress}%)`);
  assert.equal(M._terra_world_open_cancel(task), 11);
  assert.equal(M._terra_world_task_close(task), 0);
  M._tx_free(inputPtr);
  assert.equal(M._tx_native_heap_used() >>> 0, baseline);
});

test("canceling during copied or previewed work rewinds all task-owned native allocations", async () => {
  const M = await TerraWorldWasm();
  const input = fs.readFileSync(TEST_WLD);
  const inputPtr = alloc(M, input, "world input");
  const baseline = M._tx_native_heap_used() >>> 0;

  let task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  assert.equal(M._terra_world_open_step(task, 3), 10);
  assert.ok((M._tx_native_heap_used() >>> 0) > baseline, "copy stage did not own native state");
  assert.equal(M._terra_world_open_cancel(task), 11);
  assert.equal(M._terra_world_task_close(task), 0);
  assert.equal(M._tx_native_heap_used() >>> 0, baseline, "copy-stage cancel leaked native state");

  task = M._terra_world_open_begin(inputPtr, input.length);
  assert.notEqual(task, 0);
  let status = 10;
  let guard = 0;
  while (status === 10 && (M._terra_world_task_get_progress(task) >>> 0) < 35 && guard < 1000) {
    status = M._terra_world_open_step(task, 1);
    guard += 1;
  }
  assert.equal(status, 10, "task completed before preview cancellation could be exercised");
  assert.ok((M._terra_world_task_get_progress(task) >>> 0) >= 35, "task never reached preview state");
  assert.ok((M._tx_native_heap_used() >>> 0) > baseline, "preview stage did not own native state");
  assert.equal(M._terra_world_open_cancel(task), 11);
  assert.equal(M._terra_world_task_get_status(task), 11);
  assert.equal(M._terra_world_task_close(task), 0);
  M._tx_free(inputPtr);
  assert.equal(M._tx_native_heap_used() >>> 0, baseline, "preview-stage cancel leaked native state");
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
