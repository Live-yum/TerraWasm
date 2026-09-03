"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { getWorldFixturePaths } = require("./helpers/fixtures");

const ROOT = path.resolve(__dirname, "..");
const modulePath = process.env.TERRAWASM_LOW_MEMORY_MODULE
  ? path.resolve(process.env.TERRAWASM_LOW_MEMORY_MODULE)
  : path.join(ROOT, "build", "terrax_world_wasm.js");
const TerraWorldWasm = require(modulePath);
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const EXPECTED_MAX_BYTES = 160 * 1024 * 1024;

function mustAlloc(M, size, label) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `${label}: tx_malloc(${size}) returned zero`);
  return ptr;
}

function readU64(M, ptr) {
  return Number(new DataView(M.wasmMemory.buffer).getBigUint64(ptr, true));
}

function allocCString(M, value) {
  const size = M.lengthBytesUTF8(value) + 1;
  const ptr = mustAlloc(M, size, `string ${value}`);
  M.stringToUTF8(value, ptr, size);
  return ptr;
}

function openWorld(M, bytes) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle");
  let handle = 0;
  try {
    M.HEAPU8.set(bytes, inputPtr);
    const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
    assert.equal(status, 0, `open failed with status ${status}`);
    handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
    assert.notEqual(handle, 0, "world handle must be non-zero");
    return { handle, inputPtr, handlePtr };
  } catch (error) {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
    throw error;
  }
}

function closeWorld(M, opened) {
  if (!opened) return;
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

function readSection(M, handle, sectionName) {
  const namePtr = allocCString(M, sectionName);
  const sizePtr = mustAlloc(M, 8, `${sectionName} size`);
  let outputPtr = 0;
  try {
    let status = M._terra_section_get_json(handle, namePtr, 0, 0n, sizePtr);
    assert.equal(status, 0);
    const required = readU64(M, sizePtr);
    outputPtr = mustAlloc(M, required, `${sectionName} output`);
    status = M._terra_section_get_json(handle, namePtr, outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0);
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
    M._tx_free(namePtr);
  }
}

function readLastError(M) {
  const sizePtr = mustAlloc(M, 8, "error size");
  let outputPtr = 0;
  try {
    let status = M._terra_info_get_last_error_json(0, 0n, sizePtr);
    if (status !== 0) return { status };
    const required = readU64(M, sizePtr);
    if (required <= 1) return {};
    outputPtr = mustAlloc(M, required, "error output");
    status = M._terra_info_get_last_error_json(outputPtr, BigInt(required), sizePtr);
    if (status !== 0) return { status };
    const text = M.UTF8ToString(outputPtr);
    return text ? JSON.parse(text) : {};
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
  }
}

function executeRenderPreview(M, handle) {
  const namePtr = allocCString(M, "render_preview_png");
  const requestPtr = allocCString(M, JSON.stringify({ max_w: 0, max_h: 0 }));
  const sizePtr = mustAlloc(M, 8, "operation response size");
  let outputPtr = 0;
  try {
    let status = M._terra_op_execute_json(handle, namePtr, requestPtr, 0, 0n, sizePtr);
    if (status !== 0) return { status, value: null };
    const required = readU64(M, sizePtr);
    outputPtr = mustAlloc(M, required, "operation response");
    status = M._terra_op_execute_json(
      handle, namePtr, requestPtr, outputPtr, BigInt(required), sizePtr,
    );
    return { status, value: status === 0 ? JSON.parse(M.UTF8ToString(outputPtr)) : null };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
    M._tx_free(requestPtr);
    M._tx_free(namePtr);
  }
}

function probeThumbnail(M, handle) {
  const sizePtr = mustAlloc(M, 8, "thumbnail size");
  const widthPtr = mustAlloc(M, 4, "thumbnail width");
  const heightPtr = mustAlloc(M, 4, "thumbnail height");
  try {
    const status = M._terra_op_get_thumbnail_png(handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 2, "thumbnail probe must report buffer-too-small");
    return {
      required: readU64(M, sizePtr),
      width: M.HEAPU32[widthPtr >>> 2] >>> 0,
      height: M.HEAPU32[heightPtr >>> 2] >>> 0,
    };
  } finally {
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
}

async function findLargeWorld(M) {
  for (const fixturePath of getWorldFixturePaths().slice().sort()) {
    const bytes = fs.readFileSync(fixturePath);
    const opened = openWorld(M, bytes);
    try {
      const header = readSection(M, opened.handle, "header");
      if (header.maxTilesX === 8400 && header.maxTilesY === 2400) {
        return { fixturePath, bytes };
      }
    } finally {
      closeWorld(M, opened);
    }
  }
  assert.fail("missing 8400x2400 regression fixture");
}

(async () => {
  const M = await TerraWorldWasm();
  const buildInfo = JSON.parse(M.UTF8ToString(M._terra_build_info_json()));
  assert.equal(Number(buildInfo.maxMemory), EXPECTED_MAX_BYTES, "low-memory module must cap linear memory at 160 MiB");

  const fixture = await findLargeWorld(M);
  let opened;
  try {
    opened = openWorld(M, fixture.bytes);
    const result = executeRenderPreview(M, opened.handle);
    if (result.status !== 0) {
      assert.fail(`160 MiB native-size preview failed: ${JSON.stringify(readLastError(M))}`);
    }
    assert.equal(result.value?.width, 8400);
    assert.equal(result.value?.height, 2400);

    const thumbnail = probeThumbnail(M, opened.handle);
    assert.equal(thumbnail.width, 8400);
    assert.equal(thumbnail.height, 2400);
    assert.ok(thumbnail.required > PNG_SIGNATURE.length);

    const outputPtr = mustAlloc(M, thumbnail.required, "thumbnail copy");
    const sizePtr = mustAlloc(M, 8, "thumbnail copy size");
    const widthPtr = mustAlloc(M, 4, "thumbnail copy width");
    const heightPtr = mustAlloc(M, 4, "thumbnail copy height");
    try {
      const status = M._terra_op_get_thumbnail_png(
        opened.handle, outputPtr, BigInt(thumbnail.required), sizePtr, widthPtr, heightPtr,
      );
      assert.equal(status, 0);
      assert.deepEqual(
        Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + PNG_SIGNATURE.length)),
        PNG_SIGNATURE,
      );
    } finally {
      M._tx_free(heightPtr);
      M._tx_free(widthPtr);
      M._tx_free(sizePtr);
      M._tx_free(outputPtr);
    }
  } finally {
    closeWorld(M, opened);
  }

  console.log(`PASS 160 MiB full preview: ${path.basename(fixture.fixturePath)} -> 8400x2400 PNG`);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
