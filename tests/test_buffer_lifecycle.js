"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const factory = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

function memory(M) {
  const read = (name) => typeof M[name] === "function" ? Number(M[name]()) : 0;
  return {
    bridge: read("_tx_bridge_heap_used"),
    native: read("_tx_native_heap_used"),
    heap: read("_tx_heap_used"),
    wasm: read("_tx_memory_used"),
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
    path.resolve(__dirname, "..", "..", "TerraX", "wld", "copy.wld"),
  );
  return candidates.find((candidate) => candidate && fs.existsSync(candidate));
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

async function main() {
  const M = await factory();
  if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
  const baseline = memory(M);

  // Invalid uploads exercise error paths and must not leave bridge/native roots alive.
  for (let index = 0; index < 50; index += 1) {
    const invalid = Buffer.alloc(64, index & 0xff);
    const opened = openWorld(M, invalid);
    assert.notEqual(opened.status, 0, "invalid WLD unexpectedly opened");
    if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
    const current = memory(M);
    assert.equal(current.bridge, baseline.bridge, `bridge leak after failed cycle ${index + 1}`);
    assert.equal(current.native, baseline.native, `native leak after failed cycle ${index + 1}`);
  }

  const fixture = findFixture();
  if (!fixture) {
    console.log("No valid WLD fixture found; valid open/save cycles skipped.");
    console.log(JSON.stringify({ baseline, final: memory(M) }, null, 2));
    return;
  }

  let bytes = fs.readFileSync(fixture);
  let plateau = 0;
  for (let index = 0; index < 50; index += 1) {
    const opened = openWorld(M, bytes);
    assert.equal(opened.status, 0, `valid open failed at cycle ${index + 1}`);
    assert.notEqual(opened.handle, 0);
    try {
      bytes = saveWorld(M, opened.handle);
    } finally {
      assert.equal(M._terra_world_close(opened.handle), 0);
    }
    if (typeof M._tx_reclaim_transients === "function") M._tx_reclaim_transients();
    const current = memory(M);
    assert.equal(current.bridge, baseline.bridge, `bridge leak after valid cycle ${index + 1}`);
    assert.equal(current.native, baseline.native, `native leak after valid cycle ${index + 1}`);
    if (index >= 10) plateau = Math.max(plateau, current.wasm);
    if (index >= 20) {
      assert.ok(current.wasm <= plateau, `linear memory kept growing at cycle ${index + 1}`);
    }
  }

  console.log(JSON.stringify({ fixture, baseline, final: memory(M), outputBytes: bytes.length }, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
