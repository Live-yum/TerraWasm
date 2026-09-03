"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const {
  CORRUPTED_FIXTURE_DIR,
  JPEG_PATH,
  PRIMARY_WORLD_PATH,
  SECONDARY_WORLD_PATH,
  TRUNCATED_FIXTURE_DIR,
  getWorldFixturePaths,
  readFixtureDirectory,
} = require("./helpers/fixtures");

const ROOT = path.resolve(__dirname, "..");
const TerraWorldWasm = require(path.join(ROOT, "build", "terrax_world_wasm.js"));
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const REQUIRED_TEST_FILES = [
  "tests/test_all.js",
  "tests/test_batch_update_thumbnail.js",
  "tests/test_buffer_io.js",
  "tests/test_commands.js",
  "tests/test_marker_outputs.js",
  "tests/test_memory_lifecycle.js",
  "tests/test_map_streaming_contract.js",
  "tests/test_open_task.js",
  "tests/test_pixel_art.js",
  "tests/test_pixel_art_bulk.js",
  "tests/test_pixel_art_from_jpeg.js",
  "tests/test_pixel_art_indexed.js",
  "tests/test_reader_safety.js",
  "tests/test_section_mutators.js",
  "tests/test_signs.js",
  "tests/helpers/fixtures.js",
];

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
  const ptr = mustAlloc(M, length, `string ${value}`);
  M.stringToUTF8(value, ptr, length);
  return ptr;
}

function openBuffer(M, bytes) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle output");
  let handle = 0;
  try {
    M.HEAPU8.set(bytes, inputPtr);
    const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
    assert.equal(status, 0, `terra_world_open_from_buffer failed with status ${status}`);
    handle = M.HEAPU32[handlePtr >>> 2] >>> 0;
    assert.notEqual(handle, 0, "terra_world_open_from_buffer returned a zero handle");
    return { handle, inputPtr, handlePtr };
  } catch (error) {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
    throw error;
  }
}

function releaseOpenBuffer(M, opened) {
  if (!opened) return;
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

function readSectionJson(M, handle, sectionName) {
  const namePtr = allocCString(M, sectionName);
  const sizePtr = mustAlloc(M, 8, `${sectionName} size`);
  let outputPtr = 0;
  try {
    let status = M._terra_section_get_json(handle, namePtr, 0, 0n, sizePtr);
    assert.equal(status, 0, `${sectionName} size query failed with status ${status}`);
    const required = readU64(M, sizePtr);
    assert.ok(required > 1, `${sectionName} size query returned ${required}`);
    outputPtr = mustAlloc(M, required, `${sectionName} output`);
    status = M._terra_section_get_json(handle, namePtr, outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0, `${sectionName} copy failed with status ${status}`);
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
    M._tx_free(namePtr);
  }
}

function readLastErrorJson(M) {
  const sizePtr = mustAlloc(M, 8, "error size");
  let outputPtr = 0;
  try {
    let status = M._terra_info_get_last_error_json(0, 0n, sizePtr);
    assert.equal(status, 0, "last-error probe must succeed");
    const required = readU64(M, sizePtr);
    if (!required) return {};
    outputPtr = mustAlloc(M, required, "error output");
    status = M._terra_info_get_last_error_json(outputPtr, BigInt(required), sizePtr);
    assert.equal(status, 0, "last-error copy must succeed");
    return JSON.parse(M.UTF8ToString(outputPtr));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(sizePtr);
  }
}

function executeOperation(M, handle, operationName, request) {
  const namePtr = allocCString(M, operationName);
  const requestPtr = allocCString(M, JSON.stringify(request));
  const sizePtr = mustAlloc(M, 8, `${operationName} size`);
  let outputPtr = 0;
  try {
    let status = M._terra_op_execute_json(handle, namePtr, requestPtr, 0, 0n, sizePtr);
    if (status !== 0) return { status, value: null };
    const required = readU64(M, sizePtr);
    outputPtr = mustAlloc(M, required, `${operationName} output`);
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

function getThumbnailPng(M, handle) {
  const sizePtr = mustAlloc(M, 8, "thumbnail size");
  const widthPtr = mustAlloc(M, 4, "thumbnail width");
  const heightPtr = mustAlloc(M, 4, "thumbnail height");
  let outputPtr = 0;
  try {
    let status = M._terra_op_get_thumbnail_png(handle, 0, 0n, sizePtr, widthPtr, heightPtr);
    assert.equal(status, 2, "thumbnail probe must report buffer-too-small");
    const required = readU64(M, sizePtr);
    assert.ok(required > PNG_SIGNATURE.length, "thumbnail PNG must be non-empty");
    outputPtr = mustAlloc(M, required, "thumbnail output");
    status = M._terra_op_get_thumbnail_png(
      handle, outputPtr, BigInt(required), sizePtr, widthPtr, heightPtr,
    );
    assert.equal(status, 0, `thumbnail copy failed with status ${status}`);
    return {
      png: Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required)),
      width: M.HEAPU32[widthPtr >>> 2] >>> 0,
      height: M.HEAPU32[heightPtr >>> 2] >>> 0,
    };
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(heightPtr);
    M._tx_free(widthPtr);
    M._tx_free(sizePtr);
  }
}

async function find8400x2400Fixture(M) {
  const fixturePaths = getWorldFixturePaths().slice().sort((left, right) => left.localeCompare(right));
  for (const fixturePath of fixturePaths) {
    const bytes = fs.readFileSync(fixturePath);
    const opened = openBuffer(M, bytes);
    try {
      const header = readSectionJson(M, opened.handle, "header");
      if (header.maxTilesX === 8400 && header.maxTilesY === 2400) {
        return { fixturePath, bytes, header };
      }
    } finally {
      releaseOpenBuffer(M, opened);
    }
  }
  assert.fail("missing required 8400x2400 fixture in checked-in tests/*.wld");
}

test("checked-in fixtures replace sibling TerraX dependencies", () => {
  for (const relativePath of REQUIRED_TEST_FILES) {
    const source = fs.readFileSync(path.join(ROOT, relativePath), "utf8");
    assert.doesNotMatch(
      source,
      /(?:\.\.\/\.\.\/TerraX|\\\.\.\\\.\.\\TerraX|["'])TerraX(?:["'])?/,
      `${relativePath} must not depend on a sibling TerraX checkout`,
    );
  }

  assert.ok(fs.existsSync(PRIMARY_WORLD_PATH), "primary checked-in world fixture is missing");
  assert.ok(fs.existsSync(SECONDARY_WORLD_PATH), "secondary checked-in world fixture is missing");
  assert.ok(fs.existsSync(JPEG_PATH), "pixel-art JPEG fixture is missing");

  const truncatedFixtures = readFixtureDirectory(TRUNCATED_FIXTURE_DIR);
  const corruptedFixtures = readFixtureDirectory(CORRUPTED_FIXTURE_DIR);
  assert.ok(truncatedFixtures.length >= 2, "expected committed truncated fixtures");
  assert.ok(corruptedFixtures.length >= 1, "expected committed corrupted fixtures");
});

test("checked-in world fixtures include the required 8400x2400 regression world", async () => {
  const M = await TerraWorldWasm();
  await find8400x2400Fixture(M);
});

test("render_preview_png keeps native 8400x2400 output on the low-memory route", async () => {
  const lowMemorySource = fs.readFileSync(
    path.join(ROOT, "src", "terra_render_png_lowmem.c"),
    "utf8",
  );
  const cmakeSource = fs.readFileSync(path.join(ROOT, "CMakeLists.txt"), "utf8");
  assert.match(lowMemorySource, /txw_render_marked_preview_png\s*\(/);
  assert.doesNotMatch(lowMemorySource, /txw_render_preview_rgba\s*\(/);
  assert.match(
    cmakeSource,
    /txw_render_preview_png=txw_render_preview_png_legacy/,
    "legacy whole-image encoder must not remain the public entry point",
  );

  const M = await TerraWorldWasm();
  const fixture = await find8400x2400Fixture(M);
  const opened = openBuffer(M, fixture.bytes);
  try {
    const result = executeOperation(M, opened.handle, "render_preview_png", {
      max_w: 0,
      max_h: 0,
    });
    assert.equal(
      result.status,
      0,
      `native-size render_preview_png failed: ${JSON.stringify(readLastErrorJson(M))}`,
    );
    assert.equal(result.value?.width, 8400);
    assert.equal(result.value?.height, 2400);

    const thumbnail = getThumbnailPng(M, opened.handle);
    assert.equal(thumbnail.width, 8400);
    assert.equal(thumbnail.height, 2400);
    assert.deepEqual(thumbnail.png.subarray(0, PNG_SIGNATURE.length), PNG_SIGNATURE);
  } finally {
    releaseOpenBuffer(M, opened);
  }
});
