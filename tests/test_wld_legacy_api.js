"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const test = require("node:test");
const createNode = require("../build/terrax_world_wasm.js");
const createWeb = require("../build/terrax_world_wasm_web.js");
const { makeLegacyWorld } = require("./helpers/legacy-world");

function alloc(M, bytes) {
  const data = typeof bytes === "number" ? Buffer.alloc(bytes) : bytes;
  const pointer = M._tx_malloc(data.length);
  assert.ok(pointer);
  M.HEAPU8.set(data, pointer);
  return pointer;
}

function json(M, fn, ...args) {
  const required = alloc(M, 8);
  let output = 0;
  try {
    assert.equal(fn(...args, 0, 0n, required), 0);
    const size = M.HEAPU32[required >>> 2];
    output = alloc(M, size);
    assert.equal(fn(...args, output, BigInt(size), required), 0);
    return JSON.parse(M.UTF8ToString(output));
  } finally {
    if (output) M._tx_free(output);
    M._tx_free(required);
  }
}

function save(M, handle) {
  const required = alloc(M, 4);
  let output = 0;
  try {
    assert.equal(M._terra_world_save_to_buffer(handle, 0, 0, required), 0);
    const size = M.HEAPU32[required >>> 2];
    output = alloc(M, size);
    assert.equal(M._terra_world_save_to_buffer(handle, output, size, required), 0);
    return Buffer.from(M.HEAPU8.slice(output, output + size));
  } finally {
    if (output) M._tx_free(output);
    M._tx_free(required);
  }
}

const webFactory = () => createWeb({ wasmBinary: fs.readFileSync(require.resolve("../build/terrax_world_wasm_web.wasm")) });
for (const [target, factory] of [["Node", createNode], ["Web", webFactory]]) {
  test(`${target} legacy WLD synchronous and incremental opens preserve source and produce a preview`, async () => {
    const M = await factory();
    for (let version = 1; version <= 87; version++) {
      const bytes = makeLegacyWorld(version);
      const input = alloc(M, bytes);
      const handleOut = alloc(M, 4);
      const headerName = alloc(M, Buffer.from("header\0"));
      let handle = 0, task = 0;
      try {
        assert.equal(M._terra_world_open_from_buffer(input, bytes.length, handleOut), 0);
        handle = M.HEAPU32[handleOut >>> 2];
        const header = json(M, M._terra_section_get_json, handle, headerName);
        const section = name => {
          const key = alloc(M, Buffer.from(name + "\0"));
          try { return json(M, M._terra_section_get_json, handle, key); }
          finally { M._tx_free(key); }
        };
        const chests = section("chests");
        assert.equal(chests.length, 1, `v${version} chest count`);
        assert.equal(chests[0].items[0].stack, 3);
        assert.equal(chests[0].maxItems, version < 58 ? 20 : 40);
        if (version < 38) assert.equal(chests[0].items[0].legacyName, "Dirt");
        else assert.equal(chests[0].items[0].itemType, 1);
        assert.equal(section("signs")[0].text, "Legacy sign");
        assert.equal(section("npcs").townNpcs[0].npcNetId, 22);
        if (version>=31 && version<=83) assert.equal(section("npcs").townNpcs[0].givenName, "Old Guide");
        assert.deepEqual(section("tile_entities"), []);
        assert.deepEqual(section("creative_powers"), []);
        assert.deepEqual(section("bestiary"), { kills: [], sightings: [], chats: [] });
        if (version >= 7) {
          assert.equal(section("footer").worldName, "Legacy World");
          assert.equal(section("footer").worldId, 87);
        } else assert.equal(section("footer").present, false);

        assert.deepEqual(save(M, handle), bytes);
        assert.equal(M._terra_world_close(handle), 0);
        handle = 0;
        task = M._terra_world_open_begin(input, bytes.length);
        assert.ok(task);
        let status = 10;
        for (let step = 0; step < 1000 && status === 10; step++) status = M._terra_world_open_step(task, 1);
        assert.equal(status, 0, `release ${version} incremental open failed`);
        assert.equal(M._terra_world_open_finish(task, handleOut), 0);
        handle = M.HEAPU32[handleOut >>> 2];
        assert.deepEqual(json(M, M._terra_section_get_json, handle, headerName), header);
        assert.deepEqual(save(M, handle), bytes);
        const dimensions = alloc(M, 16);
        try {
          assert.equal(M._terra_op_get_thumbnail_png(handle, 0, 0n, dimensions, dimensions + 8, dimensions + 12), 2);
          assert.ok(M.HEAPU32[dimensions >>> 2] > 8);
          assert.ok(M.HEAPU32[(dimensions + 8) >>> 2] > 0);
        } finally { M._tx_free(dimensions); }
      } finally {
        if (handle) M._terra_world_close(handle);
        if (task) M._terra_world_task_close(task);
        M._tx_free(headerName); M._tx_free(handleOut); M._tx_free(input);
      }
      assert.equal(M._tx_native_heap_used(), 0);
      assert.equal(M._tx_bridge_heap_used(), 0);
    }
  });
}

test("legacy WLD rejects modern encoders without changing the original bytes", async () => {
  const M = await createNode();
  const bytes = makeLegacyWorld(87);
  const input = alloc(M, bytes), handleOut = alloc(M, 4), required = alloc(M, 8);
  let handle = 0;
  try {
    assert.equal(M._terra_world_open_from_buffer(input, bytes.length, handleOut), 0);
    handle = M.HEAPU32[handleOut >>> 2];
    const operations = [
      ["batch_update_tiles", { rules: [{ where: { type: 3 }, patch: { type: 0 } }] }],
      ["header_patch", { patch: { hardMode: true } }],
      ["replace_bestiary", { kills: [], sightings: [], chats: [] }],
    ];
    for (const [name, body] of operations) {
      const namePtr = alloc(M, Buffer.from(`${name}\0`));
      const requestPtr = alloc(M, Buffer.from(`${JSON.stringify(body)}\0`));
      try {
        assert.notEqual(M._terra_op_execute_json(handle, namePtr, requestPtr, 0, 0n, required), 0, name);
        assert.equal(json(M, M._terra_info_get_last_error_json).code, "TERRAX_NOT_SUPPORTED");
      } finally { M._tx_free(requestPtr); M._tx_free(namePtr); }
      assert.deepEqual(save(M, handle), bytes);
    }
    const rgba = alloc(M, Buffer.from([255, 0, 0, 255])), map = alloc(M, 32);
    try {
      assert.equal(M._txw_queue_pixel_art(handle, 0, 0, 1, 1, rgba, 4, map, 1, 1), -1);
      assert.equal(json(M, M._terra_info_get_last_error_json).code, "TERRAX_NOT_SUPPORTED");
    } finally { M._tx_free(map); M._tx_free(rgba); }
    assert.deepEqual(save(M, handle), bytes);
  } finally {
    if (handle) M._terra_world_close(handle);
    M._tx_free(required); M._tx_free(handleOut); M._tx_free(input);
  }
  assert.equal(M._tx_native_heap_used(), 0);
  assert.equal(M._tx_bridge_heap_used(), 0);
});


test("legacy WLD rejects truncated streams and reads historical special frames", async () => {
  const M = await createNode();
  const handleOut = alloc(M, 4);
  try {
    for (const version of [1, 7, 25, 27, 28, 37, 38, 40, 58, 59, 77, 78, 83, 84, 87]) {
      const bytes = makeLegacyWorld(version);
      const cuts = new Set([0, 3, 4, 20, bytes.length - 1, bytes.length - 8, Math.floor(bytes.length / 2)]);
      for (const cut of cuts) {
        const input = alloc(M, bytes.subarray(0, Math.max(1, cut)));
        try { assert.notEqual(M._terra_world_open_from_buffer(input, cut, handleOut), 0, `v${version} cut ${cut}`); }
        finally { M._tx_free(input); }
      }
    }
    for (const [version, tileType] of [[27,4],[28,4],[39,19],[40,19],[87,49],[87,144],[78,314]]) {
      const bytes = makeLegacyWorld(version, {tileType});
      const input = alloc(M, bytes);
      try {
        assert.equal(M._terra_world_open_from_buffer(input, bytes.length, handleOut), 0, `v${version} tile ${tileType}`);
        M._terra_world_close(M.HEAPU32[handleOut >>> 2]);
      } finally { M._tx_free(input); }
    }
  } finally { M._tx_free(handleOut); }
  assert.equal(M._tx_native_heap_used(), 0);
  assert.equal(M._tx_bridge_heap_used(), 0);
});
