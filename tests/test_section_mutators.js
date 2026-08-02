"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

const TEST_WLD = getPrimaryWorldPath();
const TEST_BYTES = fs.readFileSync(TEST_WLD);

function mustAlloc(M, size, label) {
  const ptr = M._tx_malloc(size);
  assert.notEqual(ptr, 0, `${label}: tx_malloc(${size}) returned zero`);
  return ptr;
}

function readU32(M, ptr) {
  return M.HEAPU32[ptr >>> 2] >>> 0;
}

function writeCString(M, value, label) {
  const bytes = Buffer.from(`${value}\0`, "utf8");
  const ptr = mustAlloc(M, bytes.length, label);
  M.HEAPU8.set(bytes, ptr);
  return ptr;
}

function readLastError(M) {
  const requiredPtr = mustAlloc(M, 8, "error required size");
  let outputPtr = 0;
  try {
    let status = M._terra_info_get_last_error_json(0, 0n, requiredPtr);
    assert.equal(status, 0);
    const required = readU32(M, requiredPtr);
    if (!required) return {};
    outputPtr = mustAlloc(M, required, "error output");
    status = M._terra_info_get_last_error_json(outputPtr, BigInt(required), requiredPtr);
    assert.equal(status, 0);
    return JSON.parse(Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required - 1)).toString("utf8"));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
  }
}

function executeOperation(M, handle, name, request, { expectFailure = false } = {}) {
  const namePtr = writeCString(M, name, "operation name");
  const requestPtr = writeCString(M, JSON.stringify(request), "operation request");
  const requiredPtr = mustAlloc(M, 8, "operation required size");
  let responsePtr = 0;
  try {
    let status = M._terra_op_execute_json(handle, namePtr, requestPtr, 0, 0n, requiredPtr);
    if (expectFailure) {
      assert.notEqual(status, 0, `${name} unexpectedly succeeded`);
      return { status, error: readLastError(M) };
    }
    if (status !== 0) assert.fail(`${name} probe failed: ${JSON.stringify(readLastError(M))}`);
    const required = readU32(M, requiredPtr);
    assert.ok(required > 1, `${name} must return a JSON response`);
    responsePtr = mustAlloc(M, required, "operation response");
    status = M._terra_op_execute_json(
      handle,
      namePtr,
      requestPtr,
      responsePtr,
      BigInt(required),
      requiredPtr,
    );
    if (status !== 0) assert.fail(`${name} copy failed: ${JSON.stringify(readLastError(M))}`);
    return JSON.parse(
      Buffer.from(M.HEAPU8.slice(responsePtr, responsePtr + required - 1)).toString("utf8"),
    );
  } finally {
    if (responsePtr) M._tx_free(responsePtr);
    M._tx_free(requiredPtr);
    M._tx_free(requestPtr);
    M._tx_free(namePtr);
  }
}

function openBytes(M, bytes) {
  const inputPtr = mustAlloc(M, bytes.length, "world input");
  const handlePtr = mustAlloc(M, 4, "world handle output");
  let handle = 0;
  try {
    M.HEAPU8.set(bytes, inputPtr);
    const status = M._terra_world_open_from_buffer(inputPtr, bytes.length, handlePtr);
    if (status !== 0) assert.fail(`open_from_buffer failed: ${JSON.stringify(readLastError(M))}`);
    handle = readU32(M, handlePtr);
    assert.notEqual(handle, 0);
    return { handle, inputPtr, handlePtr };
  } catch (error) {
    if (handle) M._terra_world_close(handle);
    M._tx_free(handlePtr);
    M._tx_free(inputPtr);
    throw error;
  }
}

function closeBytes(M, opened) {
  if (!opened) return;
  if (opened.handle) M._terra_world_close(opened.handle);
  M._tx_free(opened.handlePtr);
  M._tx_free(opened.inputPtr);
}

function getSection(M, handle, name) {
  const namePtr = writeCString(M, name, "section name");
  const requiredPtr = mustAlloc(M, 8, "section required size");
  let outputPtr = 0;
  try {
    let status = M._terra_section_get_json(handle, namePtr, 0, 0n, requiredPtr);
    if (status !== 0) assert.fail(`get ${name} probe failed: ${JSON.stringify(readLastError(M))}`);
    const required = readU32(M, requiredPtr);
    outputPtr = mustAlloc(M, required, "section output");
    status = M._terra_section_get_json(handle, namePtr, outputPtr, BigInt(required), requiredPtr);
    if (status !== 0) assert.fail(`get ${name} copy failed: ${JSON.stringify(readLastError(M))}`);
    return JSON.parse(Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required - 1)).toString("utf8"));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
    M._tx_free(namePtr);
  }
}

function saveBytes(M, handle) {
  const requiredPtr = mustAlloc(M, 4, "save required size");
  let outputPtr = 0;
  try {
    let status = M._terra_world_save_to_buffer(handle, 0, 0, requiredPtr);
    if (status !== 0) assert.fail(`save probe failed: ${JSON.stringify(readLastError(M))}`);
    const required = readU32(M, requiredPtr);
    outputPtr = mustAlloc(M, required, "save output");
    status = M._terra_world_save_to_buffer(handle, outputPtr, required, requiredPtr);
    if (status !== 0) assert.fail(`save copy failed: ${JSON.stringify(readLastError(M))}`);
    return Buffer.from(M.HEAPU8.slice(outputPtr, outputPtr + required));
  } finally {
    if (outputPtr) M._tx_free(outputPtr);
    M._tx_free(requiredPtr);
  }
}

function worldSections(bytes) {
  const version = bytes.readUInt32LE(0);
  const pointerStart = version >= 135 ? 24 : 4;
  const count = bytes.readUInt16LE(pointerStart);
  const starts = [];
  for (let i = 0; i < count; i += 1) starts.push(bytes.readUInt32LE(pointerStart + 2 + i * 4));
  return starts.map((start, index) => bytes.subarray(start, starts[index + 1] || bytes.length));
}

function digest(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

test("header_patch changes only whitelisted booleans and survives save/reopen", async () => {
  const M = await TerraWorldWasm();
  let opened;
  let reopened;
  try {
    opened = openBytes(M, TEST_BYTES);
    const before = getSection(M, opened.handle, "header");
    const patch = Object.fromEntries(
      Object.entries(before)
        .filter(([, value]) => typeof value === "boolean")
        .map(([key, value]) => [key, !value]),
    );
    assert.ok(Object.keys(patch).length > 90, "fixture must exercise the full header boolean whitelist");
    const response = executeOperation(M, opened.handle, "header_patch", { patch });
    assert.deepEqual(response, { status: "ok", updated: Object.keys(patch).length });

    const inMemory = getSection(M, opened.handle, "header");
    assert.deepEqual(
      Object.fromEntries(Object.keys(patch).map((key) => [key, inMemory[key]])),
      patch,
    );
    for (const key of Object.keys(before)) {
      if (!(key in patch)) assert.deepEqual(inMemory[key], before[key], `unpatched header field changed: ${key}`);
    }

    const saved = saveBytes(M, opened.handle);
    closeBytes(M, opened);
    opened = null;
    reopened = openBytes(M, saved);
    assert.deepEqual(getSection(M, reopened.handle, "header"), inMemory);
  } finally {
    closeBytes(M, reopened);
    closeBytes(M, opened);
  }
});

test("three mutators share one open world and produce one reopenable output without touching other sections", async () => {
  const M = await TerraWorldWasm();
  const chests = [
    {
      x: 123,
      y: 456,
      name: "安全箱 🧰",
      maxItems: 4,
      items: [
        { stack: 12, itemType: 1, prefix: 2 },
        null,
        { stack: 1, itemType: 50, prefix: 0 },
        null,
      ],
    },
  ];
  const bestiary = {
    kills: [{ persistentNpcId: "Terraria.Zombie", killCount: 50 }],
    sightings: [{ persistentNpcId: "Terraria.Bunny" }],
    chats: [{ persistentNpcId: "Terraria.Guide" }],
  };
  let opened;
  let reopened;
  try {
    opened = openBytes(M, TEST_BYTES);
    const beforeHeader = getSection(M, opened.handle, "header");
    executeOperation(M, opened.handle, "header_patch", { patch: { hardMode: !beforeHeader.hardMode } });
    executeOperation(M, opened.handle, "replace_chests", { chests });
    executeOperation(M, opened.handle, "replace_bestiary", bestiary);

    const saved = saveBytes(M, opened.handle);
    const beforeSections = worldSections(TEST_BYTES);
    const afterSections = worldSections(saved);
    assert.equal(afterSections.length, beforeSections.length, "pointer count changed");
    for (const index of [1, 3, 4, 5, 6, 7, 9, 10]) {
      assert.equal(digest(afterSections[index]), digest(beforeSections[index]), `section ${index} changed`);
    }

    closeBytes(M, opened);
    opened = null;
    reopened = openBytes(M, saved);
    assert.equal(getSection(M, reopened.handle, "header").hardMode, !beforeHeader.hardMode);
    assert.deepEqual(getSection(M, reopened.handle, "chests"), chests);
    assert.deepEqual(getSection(M, reopened.handle, "bestiary"), bestiary);
  } finally {
    closeBytes(M, reopened);
    closeBytes(M, opened);
  }
});

test("empty and maximum bounded section replacements round-trip", async () => {
  const M = await TerraWorldWasm();
  let opened;
  let reopened;
  try {
    opened = openBytes(M, TEST_BYTES);
    executeOperation(M, opened.handle, "replace_chests", { chests: [] });
    executeOperation(M, opened.handle, "replace_bestiary", { kills: [], sightings: [], chats: [] });
    assert.deepEqual(getSection(M, opened.handle, "chests"), []);
    assert.deepEqual(getSection(M, opened.handle, "bestiary"), { kills: [], sightings: [], chats: [] });

    const maxItems = Array.from({ length: 504 }, (_, index) =>
      index === 503 ? { stack: 32767, itemType: 1_000_000, prefix: 255 } : null,
    );
    const largeBestiary = {
      kills: Array.from({ length: 4096 }, (_, index) => ({
        persistentNpcId: `Terraria.TestNpc${index}`,
        killCount: 1_000_000,
      })),
      sightings: [],
      chats: [],
    };
    const largeRequestBytes = Buffer.byteLength(JSON.stringify(largeBestiary));
    assert.ok(largeRequestBytes > 4095, "fixture must exercise the large-request path");
    executeOperation(M, opened.handle, "replace_chests", {
      chests: [{ x: 0, y: 0, name: "x".repeat(255), maxItems: 504, items: maxItems }],
    });
    executeOperation(M, opened.handle, "replace_bestiary", largeBestiary);

    const saved = saveBytes(M, opened.handle);
    closeBytes(M, opened);
    opened = null;
    reopened = openBytes(M, saved);
    const roundTripChest = getSection(M, reopened.handle, "chests")[0];
    assert.equal(roundTripChest.items.length, 504);
    assert.deepEqual(roundTripChest.items[503], maxItems[503]);
    assert.deepEqual(getSection(M, reopened.handle, "bestiary"), largeBestiary);

    const maximumChests = Array.from({ length: 1000 }, (_, index) => ({
      x: index,
      y: 0,
      name: `chest-${index}`,
      maxItems: 0,
      items: [],
    }));
    executeOperation(M, reopened.handle, "replace_chests", { chests: maximumChests });
    const maximumChestSave = saveBytes(M, reopened.handle);
    closeBytes(M, reopened);
    reopened = openBytes(M, maximumChestSave);
    assert.deepEqual(getSection(M, reopened.handle, "chests"), maximumChests);
  } finally {
    closeBytes(M, reopened);
    closeBytes(M, opened);
  }
});

test("malformed, unknown, duplicate, oversized, and out-of-range fields fail atomically", async () => {
  const M = await TerraWorldWasm();
  let opened;
  try {
    opened = openBytes(M, TEST_BYTES);
    const cases = [
      ["header_patch", { patch: { notAHeaderField: true } }],
      ["header_patch", { patch: { hardMode: 1 } }],
      ["replace_chests", { chests: [], unknown: true }],
      ["replace_chests", { chests: [{ x: 1, y: 2, name: "x".repeat(256), maxItems: 1, items: [null] }] }],
      ["replace_chests", { chests: [{ x: 1, y: 2, name: "", maxItems: 1, items: [{ stack: 32768, itemType: 1, prefix: 0 }] }] }],
      ["replace_chests", { chests: Array.from({ length: 1001 }, (_, index) => ({ x: index, y: 0, name: "", maxItems: 0, items: [] })) }],
      ["replace_bestiary", { kills: [{ persistentNpcId: "x".repeat(256), killCount: 1 }], sightings: [], chats: [] }],
      ["replace_bestiary", { kills: [{ persistentNpcId: "Terraria.Zombie", killCount: 1_000_001 }], sightings: [], chats: [] }],
      ["replace_bestiary", { kills: Array.from({ length: 4097 }, (_, index) => ({ persistentNpcId: `Npc${index}`, killCount: 1 })), sightings: [], chats: [] }],
      ["replace_bestiary", { kills: [], sightings: [], chats: [], unknown: [] }],
    ];
    for (const [operation, request] of cases) {
      const before = saveBytes(M, opened.handle);
      const failure = executeOperation(M, opened.handle, operation, request, { expectFailure: true });
      assert.match(failure.error.code || "", /^TERRAX_(?:VALIDATION|NOT_SUPPORTED|PARSE)/);
      assert.equal(digest(saveBytes(M, opened.handle)), digest(before), `${operation} failure mutated world`);
    }

    const opPtr = writeCString(M, "replace_bestiary", "duplicate-key operation");
    const requestPtr = writeCString(
      M,
      '{"kills":[],"kills":[],"sightings":[],"chats":[]}',
      "duplicate-key request",
    );
    const requiredPtr = mustAlloc(M, 8, "duplicate-key required size");
    try {
      const status = M._terra_op_execute_json(opened.handle, opPtr, requestPtr, 0, 0n, requiredPtr);
      assert.notEqual(status, 0, "duplicate object keys must be rejected");
      assert.match(readLastError(M).code || "", /^TERRAX_(?:VALIDATION|PARSE)/);
    } finally {
      M._tx_free(requiredPtr);
      M._tx_free(requestPtr);
      M._tx_free(opPtr);
    }

    const invalidUtf8OpPtr = writeCString(M, "replace_bestiary", "invalid UTF-8 operation");
    const invalidUtf8Prefix = Buffer.from('{"kills":[{"persistentNpcId":"', "utf8");
    const invalidUtf8Suffix = Buffer.from('","killCount":1}],"sightings":[],"chats":[]}\0', "utf8");
    const invalidUtf8Request = Buffer.concat([invalidUtf8Prefix, Buffer.from([0xc0, 0xaf]), invalidUtf8Suffix]);
    const invalidUtf8RequestPtr = mustAlloc(M, invalidUtf8Request.length, "invalid UTF-8 request");
    const invalidUtf8RequiredPtr = mustAlloc(M, 8, "invalid UTF-8 required size");
    try {
      M.HEAPU8.set(invalidUtf8Request, invalidUtf8RequestPtr);
      const status = M._terra_op_execute_json(
        opened.handle,
        invalidUtf8OpPtr,
        invalidUtf8RequestPtr,
        0,
        0n,
        invalidUtf8RequiredPtr,
      );
      assert.notEqual(status, 0, "non-canonical UTF-8 must be rejected");
      assert.match(readLastError(M).code || "", /^TERRAX_(?:VALIDATION|PARSE)/);
    } finally {
      M._tx_free(invalidUtf8RequiredPtr);
      M._tx_free(invalidUtf8RequestPtr);
      M._tx_free(invalidUtf8OpPtr);
    }
  } finally {
    closeBytes(M, opened);
  }
});

test("legacy unlock_bestiary no longer reports success without changing bytes", async () => {
  const M = await TerraWorldWasm();
  let opened;
  try {
    opened = openBytes(M, TEST_BYTES);
    const before = digest(saveBytes(M, opened.handle));
    const failure = executeOperation(M, opened.handle, "unlock_bestiary", {}, { expectFailure: true });
    assert.equal(failure.status, 4, "unsupported operations must return NOT_SUPPORTED status");
    assert.equal(failure.error.code, "TERRAX_NOT_SUPPORTED");
    assert.equal(digest(saveBytes(M, opened.handle)), before);
  } finally {
    closeBytes(M, opened);
  }
});

test("native encoder OOM leaves the active world byte-identical and close reclaims all roots", async () => {
  const M = await TerraWorldWasm();
  const baseline = M._tx_heap_used() >>> 0;
  let opened;
  const fillers = [];
  let namePtr = 0;
  let requestPtr = 0;
  let requiredPtr = 0;
  try {
    opened = openBytes(M, TEST_BYTES);
    const before = digest(saveBytes(M, opened.handle));
    namePtr = writeCString(M, "replace_chests", "OOM operation name");
    requestPtr = writeCString(
      M,
      JSON.stringify({ chests: [{ x: 0, y: 0, name: "oom", maxItems: 1, items: [null] }] }),
      "OOM operation request",
    );
    requiredPtr = mustAlloc(M, 8, "OOM required size");

    for (const size of [32 << 20, 4 << 20, 512 << 10, 64 << 10, 8 << 10, 1024, 128, 16]) {
      for (;;) {
        const ptr = M._tx_malloc(size);
        if (!ptr) break;
        fillers.push(ptr);
      }
    }

    const status = M._terra_op_execute_json(opened.handle, namePtr, requestPtr, 0, 0n, requiredPtr);
    assert.notEqual(status, 0, "memory-starved encoder unexpectedly succeeded");
    while (fillers.length) M._tx_free(fillers.pop());
    assert.equal(readLastError(M).code, "TERRAX_WASM_OOM");
    assert.equal(digest(saveBytes(M, opened.handle)), before, "OOM published a partial section override");

  } finally {
    while (fillers.length) M._tx_free(fillers.pop());
    if (requiredPtr) M._tx_free(requiredPtr);
    if (requestPtr) M._tx_free(requestPtr);
    if (namePtr) M._tx_free(namePtr);
    closeBytes(M, opened);
  }
  assert.equal(M._tx_native_heap_used() >>> 0, 0);
  assert.equal(M._tx_bridge_heap_used() >>> 0, 0);
  assert.equal(M._tx_heap_used() >>> 0, baseline);
});
