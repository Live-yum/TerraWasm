"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TerraWorldWasmWeb = require(path.join(__dirname, "..", "build", "terrax_world_wasm_web.js"));

const REAL_FIXTURE = process.env.TERRAWASM_PLR_FIXTURE ||
  path.join(__dirname, "files", "烟花.plr");
const MODEL_FIXTURE = path.join(__dirname, "fixtures", "minimal-player.json");

function allocBytes(module, bytes) {
  const value = Buffer.from(bytes);
  const pointer = module._tx_malloc(Math.max(1, value.length));
  assert(pointer, "tx_malloc failed");
  module.HEAPU8.set(value, pointer);
  return pointer;
}

function allocString(module, value) {
  return allocBytes(module, Buffer.from(`${value}\0`, "utf8"));
}

function openBuffer(module, bytes) {
  const input = allocBytes(module, bytes);
  const output = module._tx_malloc(4);
  assert(output, "handle allocation failed");
  try {
    assert.equal(module._terra_player_open_from_buffer(input, bytes.length, output), 0,
      "real Terraria PLR fixture was rejected");
    const handle = module.HEAPU32[output >>> 2] >>> 0;
    assert(handle, "PLR handle was not returned");
    return handle;
  } finally {
    module._tx_free(output);
    module._tx_free(input);
  }
}

function openJson(module, value) {
  const input = allocString(module, JSON.stringify(value));
  const output = module._tx_malloc(4);
  assert(output);
  try {
    assert.equal(module._terra_player_open_json(input, output), 0);
    const handle = module.HEAPU32[output >>> 2] >>> 0;
    assert(handle);
    return handle;
  } finally {
    module._tx_free(output);
    module._tx_free(input);
  }
}

function probeJsonCache(module, handle) {
  const required = module._tx_malloc(4);
  assert(required);
  try {
    const before = module._tx_heap_used();
    assert.equal(module._terra_player_get_json(handle, 0, 0n, required), 0);
    const afterFirst = module._tx_heap_used();
    assert(afterFirst > before, "first get_json probe should populate the document JSON cache");
    assert.equal(module._terra_player_get_json(handle, 0, 0n, required), 0);
    assert.equal(module._tx_heap_used(), afterFirst,
      "second get_json probe must reuse the serialized JSON cache");
    return module.HEAPU32[required >>> 2] >>> 0;
  } finally {
    module._tx_free(required);
  }
}

function getDocumentJson(module, handle, knownSize = 0) {
  const required = module._tx_malloc(4);
  assert(required);
  let output = 0;
  try {
    let size = knownSize;
    if (!size) {
      assert.equal(module._terra_player_get_json(handle, 0, 0n, required), 0);
      size = module.HEAPU32[required >>> 2] >>> 0;
    }
    assert(size > 1);
    output = module._tx_malloc(size);
    assert(output);
    const beforeFetch = module._tx_heap_used();
    assert.equal(module._terra_player_get_json(handle, output, BigInt(size), required), 0);
    assert.equal(module._tx_heap_used(), beforeFetch,
      "get_json fetch must not serialize the document again");
    return JSON.parse(module.UTF8ToString(output, size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

function getField(module, handle, pointer) {
  const pathBuffer = allocString(module, pointer);
  const required = module._tx_malloc(4);
  let output = 0;
  try {
    assert.equal(module._terra_player_get_field_json(handle, pathBuffer, 0, 0n, required), 0);
    const size = module.HEAPU32[required >>> 2] >>> 0;
    output = module._tx_malloc(size);
    assert(output);
    assert.equal(module._terra_player_get_field_json(
      handle, pathBuffer, output, BigInt(size), required), 0);
    return JSON.parse(module.UTF8ToString(output, size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
    module._tx_free(pathBuffer);
  }
}

function setField(module, handle, pointer, value) {
  const pathBuffer = allocString(module, pointer);
  const valueBuffer = allocString(module, JSON.stringify(value));
  try {
    return module._terra_player_set_field_json(handle, pathBuffer, valueBuffer);
  } finally {
    module._tx_free(valueBuffer);
    module._tx_free(pathBuffer);
  }
}

function encode(module, handle, { expectCached = false } = {}) {
  const required = module._tx_malloc(4);
  let output = 0;
  try {
    const beforeProbe = module._tx_heap_used();
    assert.equal(module._terra_player_save_to_buffer(handle, 0, 0, required), 0);
    const afterFirstProbe = module._tx_heap_used();
    if (expectCached) {
      assert(afterFirstProbe > beforeProbe,
        "first dirty save probe should populate the encrypted output cache");
    } else {
      assert.equal(afterFirstProbe, beforeProbe,
        "clean save probe should use original encrypted bytes without allocating");
    }
    assert.equal(module._terra_player_save_to_buffer(handle, 0, 0, required), 0);
    assert.equal(module._tx_heap_used(), afterFirstProbe,
      "second save probe must reuse the existing encrypted bytes/cache");
    const size = module.HEAPU32[required >>> 2] >>> 0;
    assert(size > 0);
    output = module._tx_malloc(size);
    assert(output);
    const beforeFetch = module._tx_heap_used();
    assert.equal(module._terra_player_save_to_buffer(handle, output, size, required), 0);
    assert.equal(module._tx_heap_used(), beforeFetch,
      "save fetch must not encode/encrypt the PLR again");
    return Buffer.from(module.HEAPU8.slice(output, output + size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

function assertSemanticShape(player) {
  assert.equal(typeof player.name, "string");
  assert(player.name.length > 0, "real player name is empty");
  assert(Number.isInteger(player.version), "player version is not an integer");
  assert.equal(player.inventory.length, 58);
  assert.equal(player.armor.length, 20);
  assert.equal(player.dyes.length, 10);
  assert.equal(player.buffs.length, 44);
  assert.equal(player.loadouts.length, 3);
}

function realFixtureBytes() {
  assert.equal(fs.existsSync(REAL_FIXTURE), true,
    `real PLR fixture is required: ${REAL_FIXTURE}`);
  const source = fs.readFileSync(REAL_FIXTURE);
  assert(source.length >= 16 && source.length % 16 === 0,
    "real PLR fixture is not AES-CBC framed");
  return source;
}

test("Node opens a real Terraria PLR and reuses two-call caches", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const source = realFixtureBytes();
  let handle = 0;
  let reopened = 0;
  try {
    handle = openBuffer(module, source);
    const jsonSize = probeJsonCache(module, handle);
    const original = getDocumentJson(module, handle, jsonSize);
    assertSemanticShape(original);
    assert.equal(original.version, 326, "real fixture version changed unexpectedly");
    assert.equal(original.taxMoney, 113750, "v326 prefix byte alignment regression");
    assert.equal(original.numberOfDeathsPve, 11, "v326 death-counter alignment regression");
    assert.equal(original.voiceVariant, 2, "v326 loadout alignment regression");
    assert.equal(original.voicePitchOffset, 0, "v326 voice pitch alignment regression");
    assert.equal(original.formatExtensions, undefined,
      "reserved release-324 byte must not leak into semantic player JSON");
    assert.equal(original.armor[0].itemType, 3381, "v326 main armor layout regression");

    const clean = encode(module, handle);
    assert.deepEqual(clean, source,
      "clean real-player save must preserve the exact encrypted bytes");

    const editedName = `${original.name}-terrawasm`;
    assert.equal(setField(module, handle, "/name", editedName), 0);
    const edited = encode(module, handle, { expectCached: true });
    assert.notDeepEqual(edited, source);
    reopened = openBuffer(module, edited);
    assert.equal(getField(module, reopened, "/name"), editedName);
    assert.equal(getField(module, reopened, "/version"), original.version);
  } finally {
    if (reopened) assert.equal(module._terra_player_close(reopened), 0);
    if (handle) assert.equal(module._terra_player_close(handle), 0);
  }
  assert.equal(module._tx_heap_used(), baseline,
    "real PLR handles/caches leaked native allocations");
});

test("Web opens and edits the real Terraria PLR fixture", async () => {
  const module = await TerraWorldWasmWeb({
    wasmBinary: fs.readFileSync(path.join(__dirname, "..", "build", "terrax_world_wasm_web.wasm")),
  });
  const baseline = module._tx_heap_used();
  const source = realFixtureBytes();
  let handle = 0;
  let reopened = 0;
  try {
    handle = openBuffer(module, source);
    assert.equal(getField(module, handle, "/version"), 326);
    assert.equal(getField(module, handle, "/taxMoney"), 113750);
    assert.equal(getField(module, handle, "/voiceVariant"), 2);
    const originalName = getField(module, handle, "/name");
    assert.equal(typeof originalName, "string");
    const clean = encode(module, handle);
    assert.deepEqual(clean, source);
    assert.equal(setField(module, handle, "/name", `${originalName}-web`), 0);
    reopened = openBuffer(module, encode(module, handle, { expectCached: true }));
    assert.equal(getField(module, reopened, "/name"), `${originalName}-web`);
  } finally {
    if (reopened) module._terra_player_close(reopened);
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});

test("real-layout PLR accepts a newer release when the persisted layout is unchanged", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const source = realFixtureBytes();
  let handle = 0;
  let reopened = 0;
  try {
    handle = openBuffer(module, source);
    assert.equal(getField(module, handle, "/version"), 326);
    assert.equal(setField(module, handle, "/version", 327), 0,
      "release 327 must be accepted when it still matches the latest known layout");
    const encoded = encode(module, handle, { expectCached: true });
    reopened = openBuffer(module, encoded);
    assert.equal(getField(module, reopened, "/version"), 327);
    assert.equal(getField(module, reopened, "/taxMoney"), 113750);
    assert.equal(getField(module, reopened, "/voiceVariant"), 2);
  } finally {
    if (reopened) module._terra_player_close(reopened);
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});
