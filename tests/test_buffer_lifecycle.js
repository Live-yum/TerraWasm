"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const MiB = 1024 * 1024;
const CYCLES = Number(process.env.TERRAWASM_LIFECYCLE_CYCLES || 100);
const TRACKED_MEMORY_LIMIT = Number(process.env.TERRAWASM_TRACKED_MEMORY_LIMIT || 160 * MiB);
const NODE_LINEAR_LIMIT = Number(process.env.TERRAWASM_NODE_LINEAR_LIMIT || 160 * MiB);
const buildDirectory = process.env.TERRAWASM_BUILD_DIR || "build-ci";
const factory = require(path.join(__dirname, "..", buildDirectory, "terrax_world_wasm.js"));

function memory(M) {
  const read = (name) => typeof M[name] === "function" ? Number(M[name]()) : 0;
  return {
    bridge: read("_tx_bridge_heap_used"),
    native: read("_tx_native_heap_used"),
    heap: read("_tx_heap_used"),
    heapPeak: read("_tx_heap_peak"),
    bridgePeak: read("_tx_bridge_heap_peak"),
    nativePeak: read("_tx_native_heap_peak"),
    wasm: read("_tx_memory_used") || M.HEAPU8.byteLength,
  };
}

function alloc(M, size) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `tx_malloc(${size}) failed`);
  return ptr;
}

function findFixture() {
  const candidates = [];
  if (process.env.TERRAWASM_TEST_WLD) candidates.push(process.env.TERRAWASM_TEST_WLD);
  candidates.push(
    path.join(__dirname, "fixtures", "sample.wld"),
    path.join(__dirname, "..", "data", "sample.wld"),
  );
  return candidates.find((candidate) => candidate && fs.existsSync(candidate));
}

function createMinimalFixture() {
  const version = 88;
  const pointerCount = 11;
  const formatLength = 4 + 2 + pointerCount * 4 + 2;
  const sectionStart = 2048;
  const bytes = Buffer.alloc(sectionStart, 0);
  let offset = 0;

  bytes.writeUInt32LE(version, offset); offset += 4;
  bytes.writeUInt16LE(pointerCount, offset); offset += 2;
  bytes.writeUInt32LE(formatLength, offset); offset += 4;
  for (let index = 1; index < pointerCount; index += 1) {
    bytes.writeUInt32LE(sectionStart, offset); offset += 4;
  }
  bytes.writeUInt16LE(0, offset); offset += 2;
  assert.equal(offset, formatLength);

  offset = formatLength;
  const name = Buffer.from("TerraWasm-CI", "utf8");
  assert.ok(name.length < 0x80);
  bytes[offset++] = name.length;
  name.copy(bytes, offset); offset += name.length;

  bytes.writeInt32LE(1, offset); offset += 4;
  bytes.writeInt32LE(0, offset); offset += 4;
  bytes.writeInt32LE(1600, offset); offset += 4;
  bytes.writeInt32LE(0, offset); offset += 4;
  bytes.writeInt32LE(1200, offset); offset += 4;
  bytes.writeInt32LE(10, offset); offset += 4;
  bytes.writeInt32LE(10, offset); offset += 4;
  assert.ok(offset < sectionStart, "minimal header exceeded its section boundary");
  return bytes;
}

function openWorld(M, bytes) {
  const inputPtr = alloc(M, bytes.length || 1);
  const handlePtr = alloc(M, 4);
  try {
    if (bytes.length) M.HEAPU8.set(bytes, inputPtr);
    const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
    if (status !== 0) return { status, handle: 0 };
    return { status, handle: M.HEAPU32[handlePtr >>> 2] >>> 0 };
  } finally {
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
  }
}

function saveWorld(M, handle) {
  const requiredPtr = alloc(M, 4);
  let outputPtr = 0;
  try {
    let status = M._terra_world_save_to_buffer(handle, 0, 0, requiredPtr);
    assert.ok(status === 0 || status === 2, `save probe failed: ${status}`);
    const required = M.HEAPU32[requiredPtr >>> 2] >>> 0;
    assert.ok(required > 0, "save returned empty output");
    outputPtr = alloc(M, required);
    status = M._terra_world_save_to_buffer(handle, outputPtr, required, requiredPtr);
    assert.equal(status, 0, `save failed: ${status}`);
    return Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
  }
}

function assertLiveMemoryReturned(current, baseline, label) {
  assert.equal(current.bridge, baseline.bridge, `bridge leak after ${label}`);
  assert.equal(current.native, baseline.native, `native leak after ${label}`);
  assert.equal(current.heap, baseline.heap, `managed heap leak after ${label}`);
  assert.ok(current.wasm <= NODE_LINEAR_LIMIT, `linear memory exceeded ${NODE_LINEAR_LIMIT} after ${label}`);
}

async function main() {
  assert.ok(Number.isInteger(CYCLES) && CYCLES >= 50, "lifecycle cycles must be at least 50");
  const M = await factory();
  if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
  const baseline = memory(M);
  assert.ok(baseline.wasm <= NODE_LINEAR_LIMIT, "initial linear memory exceeds the configured node ceiling");

  let peakTracked = baseline.wasm;
  let peakWasm = baseline.wasm;
  let peakRss = process.memoryUsage().rss;

  for (let index = 0; index < 50; index += 1) {
    const invalid = Buffer.alloc(64, index & 0xff);
    const opened = openWorld(M, invalid);
    assert.notEqual(opened.status, 0, "invalid WLD unexpectedly opened");
    if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
    assertLiveMemoryReturned(memory(M), baseline, `failed cycle ${index + 1}`);
  }

  const fixturePath = findFixture();
  let bytes = fixturePath ? fs.readFileSync(fixturePath) : createMinimalFixture();
  const fixtureLabel = fixturePath || "generated:minimal-v88-11-sections";
  assert.ok(bytes.length > 0, "valid lifecycle fixture is empty");

  const probe = openWorld(M, bytes);
  assert.equal(probe.status, 0, "generated or supplied WLD fixture is not valid");
  assert.equal(M._terra_world_close(probe.handle), 0, "fixture probe close failed");
  if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
  assertLiveMemoryReturned(memory(M), baseline, "fixture probe");

  let warmupPlateau = 0;
  for (let index = 0; index < CYCLES; index += 1) {
    const previousBytes = bytes;
    const opened = openWorld(M, previousBytes);
    assert.equal(opened.status, 0, `valid open failed at cycle ${index + 1}`);
    assert.notEqual(opened.handle, 0);

    let saved;
    let activeMemory;
    try {
      saved = saveWorld(M, opened.handle);
      activeMemory = memory(M);
      peakWasm = Math.max(peakWasm, activeMemory.wasm);
    } finally {
      assert.equal(M._terra_world_close(opened.handle), 0, `close failed at cycle ${index + 1}`);
    }
    if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();

    assert.deepEqual(saved, previousBytes, `unmodified world changed at cycle ${index + 1}`);
    const current = memory(M);
    assertLiveMemoryReturned(current, baseline, `valid cycle ${index + 1}`);

    const trackedAtSaveBoundary = activeMemory.wasm + previousBytes.length + saved.length;
    peakTracked = Math.max(peakTracked, trackedAtSaveBoundary);
    peakRss = Math.max(peakRss, process.memoryUsage().rss);
    assert.ok(
      trackedAtSaveBoundary <= TRACKED_MEMORY_LIMIT,
      `tracked memory ${trackedAtSaveBoundary} exceeded ${TRACKED_MEMORY_LIMIT} at cycle ${index + 1}`,
    );

    if (index >= 10 && index < 20) warmupPlateau = Math.max(warmupPlateau, current.wasm);
    if (index >= 20) {
      assert.ok(current.wasm <= warmupPlateau, `linear memory kept growing at cycle ${index + 1}`);
    }
    bytes = saved;
  }

  const finalProbe = openWorld(M, bytes);
  assert.equal(finalProbe.status, 0, "final saved WLD cannot be reopened");
  assert.equal(M._terra_world_close(finalProbe.handle), 0, "final WLD close failed");
  if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
  const final = memory(M);
  assertLiveMemoryReturned(final, baseline, "final reopen");

  console.log(JSON.stringify({
    fixture: fixtureLabel,
    fixtureBytes: bytes.length,
    cycles: CYCLES,
    limits: {
      tracked: TRACKED_MEMORY_LIMIT,
      nodeLinear: NODE_LINEAR_LIMIT,
    },
    baseline,
    final,
    peakTracked,
    peakWasm,
    peakRss,
    outputBytes: bytes.length,
  }, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
