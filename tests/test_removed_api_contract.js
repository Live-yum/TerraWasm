"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const RETIRED_OPERATIONS = [
  "render_preview_rgba",
  "mark_chest_items_preview",
  "mark_chest_items_map",
  "convert_world_biome",
  "set_visibility",
  "remove_all_wires",
  "unlock_bestiary",
];

function mustAlloc(M, size, label) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `${label}: tx_malloc(${size}) returned zero`);
  return ptr;
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

function readLastError(M) {
  const requiredPtr = mustAlloc(M, 8, "last error size");
  let outputPtr = 0;
  try {
    assert.equal(M._terra_info_get_last_error_json(0, 0n, requiredPtr), 0);
    const required = readU64(M, requiredPtr);
    outputPtr = mustAlloc(M, required, "last error output");
    assert.equal(M._terra_info_get_last_error_json(outputPtr, BigInt(required), requiredPtr), 0);
    return JSON.parse(Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required - 1)).toString("utf8"));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
  }
}

test("retired RGBA getter is absent from every public surface", () => {
  for (const relativePath of [
    "include/terra_world.h",
    "exports.txt",
    "exports.web.txt",
    "exports.wld.txt",
    "docs/API.md",
  ]) {
    const source = fs.readFileSync(path.join(__dirname, "..", relativePath), "utf8");
    assert.doesNotMatch(source, /terra_op_get_preview_rgba|_terra_op_get_preview_rgba/);
  }
});

test("retired WLD operations are no longer dispatchable", async () => {
  const M = await TerraWorldWasm();
  const worldBytes = fs.readFileSync(getPrimaryWorldPath());
  const inputPtr = mustAlloc(M, worldBytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle");
  let handle = 0;
  try {
    M.HEAPU8.set(worldBytes, inputPtr);
    assert.equal(M._terra_world_open_from_buffer(inputPtr, worldBytes.length, handlePtr), 0);
    handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
    assert.notEqual(handle, 0);

    for (const operation of RETIRED_OPERATIONS) {
      const operationPtr = writeCString(M, operation, `${operation} name`);
      const requestPtr = writeCString(M, "{}", `${operation} request`);
      const requiredPtr = mustAlloc(M, 8, `${operation} response size`);
      try {
        const status = M._terra_op_execute_json(
          handle,
          operationPtr,
          requestPtr,
          0,
          0n,
          requiredPtr,
        );
        const error = readLastError(M);
        assert.ok(
          status === 3 || status === 4,
          `${operation} must return NOT_FOUND/NOT_SUPPORTED, got ${status}: ${JSON.stringify(error)}`,
        );
        assert.match(
          String(error.code || ""),
          /^TERRAX_(?:UNKNOWN_OPERATION|NOT_FOUND|NOT_SUPPORTED)$/,
          `${operation} must expose a retired-operation error`,
        );
      } finally {
        M._tx_free(requiredPtr);
        M._tx_free(requestPtr);
        M._tx_free(operationPtr);
      }
    }
  } finally {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
  }
});
