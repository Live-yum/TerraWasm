"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TerraWorldWasmWeb = require(path.join(__dirname, "..", "build", "terrax_world_wasm_web.js"));

const FIXTURE = process.env.TERRAWASM_PLR_FIXTURE ||
  path.join(__dirname, "..", "..", "TerraR", "players", "yanhua.plr");

function allocBytes(module, bytes) {
  const value = Buffer.from(bytes);
  const pointer = module._tx_malloc(Math.max(1, value.length));
  assert(pointer, "tx_malloc failed");
  module.HEAPU8.set(value, pointer);
  return pointer;
}

function allocString(module, value) {
  const bytes = Buffer.from(`${value}\0`, "utf8");
  const pointer = allocBytes(module, bytes);
  return pointer;
}

function openBuffer(module, bytes) {
  const input = allocBytes(module, bytes);
  const output = module._tx_malloc(4);
  assert(output, "handle allocation failed");
  try {
    assert.equal(module._terra_player_open_from_buffer(input, bytes.length, output), 0);
    const handle = module.HEAPU32[output >>> 2] >>> 0;
    assert(handle, "PLR handle was not returned");
    return handle;
  } finally {
    module._tx_free(input);
    module._tx_free(output);
  }
}

function twoCallJson(module, fn, prefix) {
  const required = module._tx_malloc(4);
  assert(required, "required-size allocation failed");
  let output = 0;
  try {
    assert.equal(fn(...prefix, 0, 0n, required), 0, "PLR JSON probe failed");
    const size = module.HEAPU32[required >>> 2] >>> 0;
    assert(size > 1, "PLR JSON required size is empty");
    output = module._tx_malloc(size);
    assert(output, "PLR JSON output allocation failed");
    assert.equal(fn(...prefix, output, BigInt(size), required), 0, "PLR JSON fetch failed");
    return JSON.parse(module.UTF8ToString(output, size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

function getField(module, handle, pointer) {
  const pointerBuffer = allocString(module, pointer);
  try {
    return twoCallJson(module, module._terra_player_get_field_json, [handle, pointerBuffer]);
  } finally {
    module._tx_free(pointerBuffer);
  }
}

function setField(module, handle, pointer, value) {
  const pointerBuffer = allocString(module, pointer);
  const valueBuffer = allocString(module, JSON.stringify(value));
  try {
    return module._terra_player_set_field_json(handle, pointerBuffer, valueBuffer);
  } finally {
    module._tx_free(valueBuffer);
    module._tx_free(pointerBuffer);
  }
}

function encode(module, handle) {
  const required = module._tx_malloc(4);
  assert(required, "PLR output-size allocation failed");
  let output = 0;
  try {
    assert.equal(module._terra_player_save_to_buffer(handle, 0, 0, required), 0);
    const size = module.HEAPU32[required >>> 2] >>> 0;
    assert(size > 0, "PLR encoded size is empty");
    output = module._tx_malloc(size);
    assert(output, "PLR output allocation failed");
    assert.equal(module._terra_player_save_to_buffer(handle, output, size, required), 0);
    return Buffer.from(module.HEAPU8.slice(output, output + size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

test("Node PLR ABI reads, edits, encrypts, and reopens a Terraria player", {
  skip: !fs.existsSync(FIXTURE),
}, async () => {
  const source = fs.readFileSync(FIXTURE);
  const module = await TerraWorldWasm();
  const originalHeap = module._tx_heap_used();
  let handle = 0;
  let reopened = 0;
  let pathHandle = 0;
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "terrawasm-plr-"));
  const inputPath = path.join(tempDir, "input.plr");
  const outputPath = path.join(tempDir, "output.plr");

  try {
    handle = openBuffer(module, source);
    assert(module._tx_heap_used() > originalHeap, "PLR allocations are not tracked");

    const original = twoCallJson(module, module._terra_player_get_json, [handle]);
    assert.equal(original.name, "yanhua");
    assert.equal(getField(module, handle, "/name"), "yanhua");
    assert.deepEqual(encode(module, handle), source, "clean save must preserve bytes");

    assert.equal(setField(module, handle, "/name", "terrawasm-node"), 0);
    assert.equal(getField(module, handle, "/name"), "terrawasm-node");

    const invalidEdits = allocString(module,
      JSON.stringify([
        { path: "/name", value: "must-not-commit" },
        { path: "/doesNotExist", value: 1 },
      ]));
    try {
      assert.equal(module._terra_plr_set_many(handle, invalidEdits), 6);
    } finally {
      module._tx_free(invalidEdits);
    }
    assert.equal(getField(module, handle, "/name"), "terrawasm-node",
      "failed set_many must be atomic");

    const patch = allocString(module, JSON.stringify({
      items: [{ section: "inventory", index: 0, itemType: 1234, stack: 2, prefix: 3, favorited: true }],
      buffs: [{ index: 0, buffType: 5, buffTime: 60 }],
      loadoutSlots: [
        { loadoutIndex: 0, slotKind: "Armor", slotIndex: 0, itemType: 4321 },
        { loadoutIndex: 0, slotKind: "Dye", slotIndex: 0,
          itemType: null, stack: null, prefix: null },
        { loadoutIndex: 0, slotKind: "Hide", slotIndex: 0, hide: null },
      ],
    }));
    try {
      assert.equal(module._terra_player_apply_patch_json(handle, patch), 0);
    } finally {
      module._tx_free(patch);
    }

    const edited = encode(module, handle);
    assert.notDeepEqual(edited, source, "edited save must not preserve original bytes");
    reopened = openBuffer(module, edited);
    assert.equal(getField(module, reopened, "/name"), "terrawasm-node");
    assert.equal(getField(module, reopened, "/inventory/0/itemType"), 1234);
    assert.equal(getField(module, reopened, "/loadouts/0/dyes/0/itemType"), 0);
    assert.equal(getField(module, reopened, "/loadouts/0/dyes/0/stack"), 1);
    assert.equal(getField(module, reopened, "/loadouts/0/dyes/0/prefix"), 0);
    assert.equal(getField(module, reopened, "/loadouts/0/hide/0"), false);

    const escapedInputValue = { ...original, "x~y/z": { value: 7 } };
    const escapedInput = allocString(module, JSON.stringify(escapedInputValue));
    const escapedOutput = module._tx_malloc(4);
    assert(escapedOutput);
    let escapedHandle = 0;
    try {
      assert.equal(module._terra_player_open_json(escapedInput, escapedOutput), 0);
      escapedHandle = module.HEAPU32[escapedOutput >>> 2] >>> 0;
      assert.equal(getField(module, escapedHandle, "/x~0y~1z/value"), 7);
      assert.equal(setField(module, escapedHandle, "/x~0y~1z/value", 8), 0);
      assert.equal(getField(module, escapedHandle, "/x~0y~1z/value"), 8);
    } finally {
      if (escapedHandle) assert.equal(module._terra_player_close(escapedHandle), 0);
      module._tx_free(escapedOutput);
      module._tx_free(escapedInput);
    }

    const jsonInput = allocString(module, JSON.stringify(original));
    const jsonOutput = module._tx_malloc(4);
    assert(jsonOutput);
    try {
      assert.equal(module._terra_player_open_json(jsonInput, jsonOutput), 0);
      const jsonHandle = module.HEAPU32[jsonOutput >>> 2] >>> 0;
      assert(jsonHandle);
      assert.equal(getField(module, jsonHandle, "/name"), "yanhua");
      assert.equal(module._terra_player_close(jsonHandle), 0);
    } finally {
      module._tx_free(jsonOutput);
      module._tx_free(jsonInput);
    }

    fs.writeFileSync(inputPath, source);
    const inputPathBuffer = allocString(module, inputPath);
    const outputPathBuffer = allocString(module, outputPath);
    const pathOutput = module._tx_malloc(4);
    assert(pathOutput);
    try {
      assert.equal(module._terra_plr_open(inputPathBuffer, pathOutput), 0);
      pathHandle = module.HEAPU32[pathOutput >>> 2] >>> 0;
      assert(pathHandle);
      assert.equal(module._terra_plr_save(pathHandle, outputPathBuffer), 0);
      assert.deepEqual(fs.readFileSync(outputPath), source);
    } finally {
      if (pathHandle) {
        assert.equal(module._terra_player_close(pathHandle), 0);
        pathHandle = 0;
      }
      module._tx_free(pathOutput);
      module._tx_free(outputPathBuffer);
      module._tx_free(inputPathBuffer);
    }

    const malformed = Buffer.from(source);
    malformed[malformed.length - 1] ^= 1;
    const malformedCases = [
      Buffer.alloc(0),
      source.subarray(0, 1),
      source.subarray(0, 15),
      source.subarray(0, 16),
      malformed,
    ];
    for (const candidate of malformedCases) {
      const beforeFailure = module._tx_heap_used();
      const badInput = allocBytes(module, candidate);
      const badOutput = module._tx_malloc(4);
      try {
        const status = module._terra_player_open_from_buffer(
          badInput, candidate.length, badOutput);
        assert.notEqual(status, 0, "malformed PLR unexpectedly opened");
      } finally {
        module._tx_free(badOutput);
        module._tx_free(badInput);
      }
      assert.equal(module._tx_heap_used(), beforeFailure,
        "failed PLR open leaked native allocations");
    }
  } finally {
    if (reopened) module._terra_player_close(reopened);
    if (handle) module._terra_player_close(handle);
    fs.rmSync(tempDir, { recursive: true, force: true });
  }

  assert.equal(module._tx_heap_used(), originalHeap, "closing PLR handles must release allocations");
});

test("Web PLR ABI processes buffer and virtual-FS player files", {
  skip: !fs.existsSync(FIXTURE),
}, async () => {
  const source = fs.readFileSync(FIXTURE);
  const module = await TerraWorldWasmWeb({
    wasmBinary: fs.readFileSync(path.join(__dirname, "..", "build", "terrax_world_wasm_web.wasm")),
  });
  const originalHeap = module._tx_heap_used();
  let handle = 0;
  let pathHandle = 0;
  try {
    handle = openBuffer(module, source);
    assert.equal(getField(module, handle, "/name"), "yanhua");
    assert.equal(setField(module, handle, "/name", "terrawasm-web"), 0);
    const edited = encode(module, handle);
    const reopened = openBuffer(module, edited);
    assert.equal(getField(module, reopened, "/name"), "terrawasm-web");
    assert.equal(module._terra_player_close(reopened), 0);

    module.FS.writeFile("/input.plr", source);
    const inputPath = allocString(module, "/input.plr");
    const outputPath = allocString(module, "/output.plr");
    const outputHandle = module._tx_malloc(4);
    assert(outputHandle);
    try {
      assert.equal(module._terra_plr_open(inputPath, outputHandle), 0);
      pathHandle = module.HEAPU32[outputHandle >>> 2] >>> 0;
      assert(pathHandle);
      assert.equal(module._terra_plr_save(pathHandle, outputPath), 0);
      assert.deepEqual(Buffer.from(module.FS.readFile("/output.plr")), source);
    } finally {
      module._tx_free(outputHandle);
      module._tx_free(outputPath);
      module._tx_free(inputPath);
    }
  } finally {
    if (pathHandle) module._terra_plr_close(pathHandle);
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), originalHeap, "Web PLR handles leaked allocations");
});
