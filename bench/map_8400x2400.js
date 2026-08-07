"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { performance } = require("node:perf_hooks");
const { getWorldFixturePaths } = require("../tests/helpers/fixtures");

const ROOT = path.resolve(__dirname, "..");
const MiB = 1024 * 1024;
const TerraWorldWasm = require(path.join(ROOT, "build", "terrax_world_wasm.js"));

function mustAlloc(M, size, label) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `${label}: tx_malloc(${size}) returned zero`);
  return ptr;
}

function readU64(M, ptr) {
  return Number(new DataView(M.wasmMemory.buffer).getBigUint64(ptr, true));
}

function allocCString(M, value) {
  const length = M.lengthBytesUTF8(value) + 1;
  const ptr = mustAlloc(M, length, value);
  M.stringToUTF8(value, ptr, length);
  return ptr;
}

function readHeader(M, handle) {
  const namePtr = allocCString(M, "header");
  const sizePtr = mustAlloc(M, 8, "header size");
  let outputPtr = 0;
  try {
    assert.equal(M._terra_section_get_json(handle, namePtr, 0, 0n, sizePtr), 0);
    const size = readU64(M, sizePtr);
    outputPtr = mustAlloc(M, size, "header output");
    assert.equal(M._terra_section_get_json(handle, namePtr, outputPtr, BigInt(size), sizePtr), 0);
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
    M._tx_free(namePtr);
  }
}

function openBuffer(M, bytes) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle");
  M.HEAPU8.set(bytes, inputPtr);
  const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
  if (status !== 0) {
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
    return null;
  }
  return {
    handle: M.HEAPU32[handlePtr >>> 2] >>> 0,
    inputPtr,
    handlePtr,
  };
}

function closeBuffer(M, opened) {
  if (!opened) return;
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

function findLargeWorld(M) {
  for (const fixturePath of getWorldFixturePaths()) {
    const bytes = fs.readFileSync(fixturePath);
    const opened = openBuffer(M, bytes);
    if (!opened) continue;
    const header = readHeader(M, opened.handle);
    if (header.maxTilesX === 8400 && header.maxTilesY === 2400) {
      return { fixturePath, bytes, opened };
    }
    closeBuffer(M, opened);
  }
  throw new Error("checked-in 8400x2400 WLD fixture is missing");
}

function executeMap(M, handle) {
  const operationPtr = allocCString(M, "render_lit_map");
  const requestPtr = allocCString(M, "{}");
  const sizePtr = mustAlloc(M, 8, "operation result size");
  let responsePtr = 0;
  try {
    const started = performance.now();
    const status = M._terra_op_execute_json(handle, operationPtr, requestPtr, 0, 0n, sizePtr);
    const elapsedMs = performance.now() - started;
    assert.equal(status, 0, `render_lit_map failed with status ${status}`);

    const responseSize = readU64(M, sizePtr);
    if (responseSize > 0) {
      responsePtr = mustAlloc(M, responseSize, "operation response");
      assert.equal(
        M._terra_op_execute_json(
          handle,
          operationPtr,
          requestPtr,
          responsePtr,
          BigInt(responseSize),
          sizePtr,
        ),
        0,
      );
    }

    const mapSizePtr = mustAlloc(M, 8, "map size");
    const widthPtr = mustAlloc(M, 4, "map width");
    const heightPtr = mustAlloc(M, 4, "map height");
    try {
      const mapStatus = M._terra_op_get_map(handle, 0, 0n, mapSizePtr, widthPtr, heightPtr);
      assert.ok(mapStatus === 0 || mapStatus === 2, `map probe failed with status ${mapStatus}`);
      return {
        elapsedMs,
        mapBytes: readU64(M, mapSizePtr),
        width: M.HEAPU32[widthPtr >>> 2] >>> 0,
        height: M.HEAPU32[heightPtr >>> 2] >>> 0,
      };
    } finally {
      M._tx_free(heightPtr);
      M._tx_free(widthPtr);
      M._tx_free(mapSizePtr);
    }
  } finally {
    if (responsePtr) M._tx_free(responsePtr);
    M._tx_free(sizePtr);
    M._tx_free(requestPtr);
    M._tx_free(operationPtr);
  }
}

async function main() {
  const M = await TerraWorldWasm();
  const { fixturePath, bytes, opened } = findLargeWorld(M);
  try {
    const result = executeMap(M, opened.handle);
    assert.equal(result.width, 8400);
    assert.equal(result.height, 2400);
    assert.ok(result.mapBytes > 0);

    const report = {
      fixture: path.relative(ROOT, fixturePath).replaceAll(path.sep, "/"),
      worldBytes: bytes.length,
      dimensions: "8400x2400",
      elapsedMs: Number(result.elapsedMs.toFixed(1)),
      mapBytes: result.mapBytes,
      wasmLinearBytes: Number(M._tx_memory_used()),
      heapUsedBytes: Number(M._tx_heap_used()),
      heapPeakBytes: Number(M._tx_heap_peak()),
      nativePeakBytes: Number(M._tx_native_heap_peak()),
      bridgePeakBytes: Number(M._tx_bridge_heap_peak()),
    };
    assert.ok(
      report.nativePeakBytes < 64 * MiB,
      `8400x2400 MAP native peak must stay below 64 MiB, got ${report.nativePeakBytes}`,
    );
    assert.ok(
      report.heapPeakBytes < 80 * MiB,
      `8400x2400 MAP tracked heap peak must stay below 80 MiB, got ${report.heapPeakBytes}`,
    );
    console.log(`[map-benchmark] ${JSON.stringify(report)}`);
  } finally {
    closeBuffer(M, opened);
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
