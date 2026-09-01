"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TerraWorldWasmWeb = require(path.join(__dirname, "..", "build", "terrax_world_wasm_web.js"));

const EXTERNAL_FIXTURE = process.env.TERRAWASM_PLR_FIXTURE || "";
const MODEL_FIXTURE = path.join(__dirname, "fixtures", "minimal-player.json");

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

function openJson(module, value) {
  const input = allocString(module, JSON.stringify(value));
  const output = module._tx_malloc(4);
  assert(output, "JSON handle allocation failed");
  try {
    assert.equal(module._terra_player_open_json(input, output), 0,
      "semantic PLR fixture was rejected");
    const handle = module.HEAPU32[output >>> 2] >>> 0;
    assert(handle, "JSON PLR handle was not returned");
    return handle;
  } finally {
    module._tx_free(output);
    module._tx_free(input);
  }
}

function fixtureBytes(module) {
  if (EXTERNAL_FIXTURE) {
    assert.equal(fs.existsSync(EXTERNAL_FIXTURE), true,
      `TERRAWASM_PLR_FIXTURE does not exist: ${EXTERNAL_FIXTURE}`);
    return fs.readFileSync(EXTERNAL_FIXTURE);
  }
  const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  const handle = openJson(module, model);
  try {
    return encode(module, handle);
  } finally {
    assert.equal(module._terra_player_close(handle), 0);
  }
}

test("Node PLR ABI creates, reads, edits, encrypts, and reopens a Terraria player", async () => {
  const module = await TerraWorldWasm();
  const originalHeap = module._tx_heap_used();
  const source = fixtureBytes(module);
  assert.equal(module._tx_heap_used(), originalHeap,
    "building the self-contained PLR fixture leaked allocations");
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
    assert.equal(typeof original.name, "string");
    assert.notEqual(original.name.length, 0);
    if (EXTERNAL_FIXTURE) {
      assert.equal(original.version, 326, "real fixture version regression");
      assert.equal(original.taxMoney, 113750, "v326 body-prefix alignment regression");
      assert.equal(original.numberOfDeathsPve, 11, "v326 death alignment regression");
      assert.equal(original.voiceVariant, 2, "v326 loadout alignment regression");
    }
    assert.equal(getField(module, handle, "/name"), original.name);
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
      assert.equal(getField(module, jsonHandle, "/name"), original.name);
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

test("Web PLR ABI creates and processes buffer and virtual-FS player files", async () => {
  const module = await TerraWorldWasmWeb({
    wasmBinary: fs.readFileSync(path.join(__dirname, "..", "build", "terrax_world_wasm_web.wasm")),
  });
  const originalHeap = module._tx_heap_used();
  const source = fixtureBytes(module);
  assert.equal(module._tx_heap_used(), originalHeap,
    "building the Web PLR fixture leaked allocations");
  let handle = 0;
  let pathHandle = 0;
  try {
    handle = openBuffer(module, source);
    if (EXTERNAL_FIXTURE) {
      const original = twoCallJson(module, module._terra_player_get_json, [handle]);
      assert.equal(original.version, 326);
      assert.equal(original.taxMoney, 113750);
      assert.equal(original.voiceVariant, 2);
    }
    const originalName = getField(module, handle, "/name");
    assert.equal(typeof originalName, "string");
    assert.notEqual(originalName.length, 0);
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


function makeAirItem() {
  return { itemType: 0, stack: 0, prefix: 0, favorited: false };
}

function historicalModel(version) {
  const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  model.version = version;
  if (version < 135) model.metadata = null;
  const clearItems = (items) => items.map(() => makeAirItem());
  model.armor = clearItems(model.armor);
  model.dyes = clearItems(model.dyes);
  model.inventory = clearItems(model.inventory);
  model.miscEquips = clearItems(model.miscEquips);
  model.miscDyes = clearItems(model.miscDyes);
  model.piggyBank = clearItems(model.piggyBank);
  model.safe = clearItems(model.safe);
  model.defendersForge = clearItems(model.defendersForge);
  model.voidVault = clearItems(model.voidVault);
  model.pendingRefunds = [];
  model.oneTimeDialoguesSeen = [];
  model.creativeItemSacrifices = [];
  model.creativeTrackerHasNewUnlocks = false;
  model.temporarySlots = [null, null, null, null];
  for (const loadout of model.loadouts) {
    loadout.armor = clearItems(loadout.armor);
    loadout.dyes = clearItems(loadout.dyes);
    loadout.hide = loadout.hide.map(() => false);
  }
  model.buffs = model.buffs.map(() => ({ buffType: 0, buffTime: 0 }));
  const builderCount = version < 164 ? 0 : version < 167 ? 8 : version < 197 ? 10 : version < 230 ? 11 : 12;
  model.tailLayout.builderAccStatusCount = builderCount;
  model.tailLayout.includesDeathMetadata = version >= 200;
  model.builderAccStatus = Array(builderCount || 12).fill(0);
  return model;
}

function decryptPlr(bytes) {
  const key = Buffer.from([104,0,51,0,121,0,95,0,103,0,85,0,121,0,90,0]);
  const decipher = crypto.createDecipheriv("aes-128-cbc", key, key);
  return Buffer.concat([decipher.update(bytes), decipher.final()]);
}

function encryptPlr(plain) {
  const key = Buffer.from([104,0,51,0,121,0,95,0,103,0,85,0,121,0,90,0]);
  const cipher = crypto.createCipheriv("aes-128-cbc", key, key);
  return Buffer.concat([cipher.update(plain), cipher.final()]);
}

function lastError(module) {
  assert.equal(typeof module._terra_info_get_last_error_json, "function",
    "structured last-error ABI must be exported by the selected feature set");
  const required = module._tx_malloc(8);
  assert(required, "last-error required-size allocation failed");
  let output = 0;
  try {
    assert.equal(module._terra_info_get_last_error_json(0, 0n, required), 0);
    const view = new DataView(module.HEAPU8.buffer);
    const size = Number(view.getBigUint64(required, true));
    assert(size > 1, "last-error JSON is empty");
    output = module._tx_malloc(size);
    assert(output, "last-error output allocation failed");
    assert.equal(module._terra_info_get_last_error_json(output, BigInt(size), required), 0);
    return JSON.parse(module.UTF8ToString(output, size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

test("PLR recognizes every historical Terraria release 1-326", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  for (let version = 1; version <= 326; version++) {
    let handle = 0;
    let reopened = 0;
    try {
      handle = openJson(module, historicalModel(version));
      const encoded = encode(module, handle);
      reopened = openBuffer(module, encoded);
      assert.equal(getField(module, reopened, "/version"), version, `release ${version}`);
      assert.equal(typeof getField(module, reopened, "/name"), "string", `release ${version}`);
    } finally {
      if (reopened) assert.equal(module._terra_player_close(reopened), 0);
      if (handle) assert.equal(module._terra_player_close(handle), 0);
    }
    assert.equal(module._tx_heap_used(), baseline, `release ${version} leaked tracked allocations`);
  }
});

test("PLR historical source gates keep old layouts aligned", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const versions = [
    9,10,16,17,37,38,46,47,57,58,73,74,80,81,82,83,97,98,106,107,
    113,114,116,117,118,119,123,124,125,127,128,134,135,137,138,
    161,162,163,164,166,167,180,181,182,196,197,198,199,200,201,202,
    205,206,213,214,217,218,219,220,228,229,230,251,252,253,254,255,
    256,259,260,261,262,279,280,281,282,283,299,300,309,310,321,322,323,324,325,326,
  ];
  for (const version of versions) {
    const model = historicalModel(version);
    model.difficulty = 2;
    model.playTimeTicks = 123456;
    model.hairDye = 7;
    model.team = 5;
    model.hideVisibleAccessory[0] = true;
    model.hideVisibleAccessory[8] = true;
    model.hideMisc = 3;
    model.skinVariant = 4;
    model.extraAccessory = true;
    model.unlockedBiomeTorches = true;
    model.usingBiomeTorches = true;
    model.ateArtisanBread = true;
    model.usedAegisCrystal = true;
    model.downedDd2EventAnyDifficulty = true;
    model.taxMoney = 99;
    model.numberOfDeathsPve = 7;
    model.numberOfDeathsPvp = 8;
    model.hbLocked = true;
    model.anglerQuestsFinished = 12;
    model.bartenderQuestLog = 13;
    model.dead = true;
    model.respawnTimer = 42;
    model.lastSaveUtcTicks = 987654;
    model.golferScoreAccumulated = 55;
    model.unlockedSuperCart = true;
    model.enabledSuperCart = true;
    model.currentLoadoutIndex = 2;
    model.voiceVariant = 3;
    model.voicePitchOffset = 0.25;
    model.pendingRefunds = version >= 300 ? [{ itemType: 1, stack: 2, prefix: 0, favorited: false }] : [];
    model.oneTimeDialoguesSeen = version >= 310 ? ["gate"] : [];
    let handle = 0, reopened = 0;
    try {
      handle = openJson(module, model);
      reopened = openBuffer(module, encode(module, handle));
      assert.equal(getField(module, reopened, "/difficulty"), version >= 10 ? 2 : 0);
      assert.equal(getField(module, reopened, "/playTimeTicks"), version >= 138 ? 123456 : 0);
      assert.equal(getField(module, reopened, "/hairDye"), version >= 82 ? 7 : 0);
      assert.equal(getField(module, reopened, "/team"), version >= 283 ? 5 : 0);
      assert.equal(getField(module, reopened, "/hideVisibleAccessory/0"), version >= 83);
      assert.equal(getField(module, reopened, "/hideVisibleAccessory/8"), version >= 124);
      assert.equal(getField(module, reopened, "/hideMisc"), version >= 119 ? 3 : 0);
      assert.equal(getField(module, reopened, "/extraAccessory"), version >= 125);
      assert.equal(getField(module, reopened, "/taxMoney"), version >= 128 ? 99 : 0);
      assert.equal(getField(module, reopened, "/downedDd2EventAnyDifficulty"), version >= 182);
      assert.equal(getField(module, reopened, "/numberOfDeathsPve"), version >= 254 ? 7 : 0);
      assert.equal(getField(module, reopened, "/hbLocked"), version >= 16);
      assert.equal(getField(module, reopened, "/anglerQuestsFinished"), version >= 98 ? 12 : 0);
      assert.equal(getField(module, reopened, "/bartenderQuestLog"), version >= 181 ? 13 : 0);
      assert.equal(getField(module, reopened, "/dead"), version >= 200);
      assert.equal(getField(module, reopened, "/lastSaveUtcTicks"), version >= 202 ? 987654 : 0);
      assert.equal(getField(module, reopened, "/golferScoreAccumulated"), version >= 206 ? 55 : 0);
      assert.equal(getField(module, reopened, "/unlockedSuperCart"), version >= 253);
      assert.equal(getField(module, reopened, "/currentLoadoutIndex"), version >= 262 ? 2 : 0);
      const expectedVoiceVariant = version >= 280 ? 3 :
        (version <= 17 ? ([5, 6, 9, 11].includes(model.hair) ? 2 : 1) : 2);
      assert.equal(getField(module, reopened, "/voiceVariant"), expectedVoiceVariant);
      assert.equal(getField(module, reopened, "/voicePitchOffset"), version >= 281 ? 0.25 : 0);
      assert.equal(getField(module, reopened, "/pendingRefunds").length, version >= 300 ? 1 : 0);
      assert.equal(getField(module, reopened, "/oneTimeDialoguesSeen").length, version >= 310 ? 1 : 0);
    } finally {
      if (reopened) module._terra_player_close(reopened);
      if (handle) module._terra_player_close(handle);
    }
    assert.equal(module._tx_heap_used(), baseline, `gate release ${version} leaked`);
  }
});

test("PLR accepts newer releases when the 326 layout is unchanged and diagnoses changed layouts", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  for (const version of [327, 400]) {
    let handle = 0, reopened = 0;
    try {
      handle = openJson(module, historicalModel(version));
      const encoded = encode(module, handle);
      reopened = openBuffer(module, encoded);
      assert.equal(getField(module, reopened, "/version"), version);
    } finally {
      if (reopened) module._terra_player_close(reopened);
      if (handle) module._terra_player_close(handle);
    }
  }

  let handle = 0;
  try {
    handle = openJson(module, historicalModel(327));
    const encoded = encode(module, handle);
    const plain = decryptPlr(encoded);
    const changedLayout = encryptPlr(Buffer.concat([plain, Buffer.from([0x7f])]));
    const input = allocBytes(module, changedLayout);
    const output = module._tx_malloc(4);
    try {
      assert.notEqual(module._terra_player_open_from_buffer(input, changedLayout.length, output), 0);
      const error = lastError(module);
      assert.equal(error.code, "TERRAX_PLR_NEWER_LAYOUT_ERROR");
      assert.match(error.message, /newer.*326.*layout/i);
    } finally {
      module._tx_free(output);
      module._tx_free(input);
    }
  } finally {
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});

test("PLR legacy item edits reject fields that old releases cannot represent", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();

  function probeSave(handle) {
    const required = module._tx_malloc(4);
    assert(required);
    try {
      return module._terra_player_save_to_buffer(handle, 0, 0, required);
    } finally {
      module._tx_free(required);
    }
  }

  const model = historicalModel(37);
  model.armor[0] = {
    itemType: 0, stack: 1, prefix: 2, favorited: false, legacyName: "Iron Pickaxe",
  };
  let handle = 0, reopened = 0;
  try {
    handle = openJson(module, model);
    reopened = openBuffer(module, encode(module, handle));
    assert.equal(getField(module, reopened, "/armor/0/legacyName"), "Iron Pickaxe");
    assert.equal(getField(module, reopened, "/armor/0/itemType"), 0);
    assert.equal(getField(module, reopened, "/armor/0/stack"), 1);
    assert.equal(getField(module, reopened, "/armor/0/prefix"), 2);

    assert.equal(setField(module, reopened, "/armor/0/itemType", 1), 0);
    assert.notEqual(probeSave(reopened), 0,
      "numeric itemType edit must not be silently ignored for release 1-37");
    assert.equal(setField(module, reopened, "/armor/0/itemType", 0), 0);
    assert.equal(setField(module, reopened, "/armor/0/stack", 0), 0);
    assert.notEqual(probeSave(reopened), 0,
      "equipment stack edit must not be silently ignored for release 1-37");
  } finally {
    if (reopened) module._terra_player_close(reopened);
    if (handle) module._terra_player_close(handle);
  }

  const v35 = historicalModel(35);
  v35.armor[0] = {
    itemType: 0, stack: 1, prefix: 1, favorited: false, legacyName: "Iron Pickaxe",
  };
  const input = allocString(module, JSON.stringify(v35));
  const output = module._tx_malloc(4);
  assert(output);
  let oldHandle = 0;
  try {
    assert.equal(module._terra_player_open_json(input, output), 0);
    oldHandle = module.HEAPU32[output >>> 2] >>> 0;
    assert(oldHandle);
    assert.notEqual(probeSave(oldHandle), 0,
      "release 1-35 prefix edit must not be silently ignored");
  } finally {
    if (oldHandle) module._terra_player_close(oldHandle);
    module._tx_free(output);
    module._tx_free(input);
  }
  assert.equal(module._tx_heap_used(), baseline);
});
