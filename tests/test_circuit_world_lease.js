'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { makeCircuitWorld } = require('./helpers/circuit-world');

test('circuit source lease blocks retirement and adoption, retains READY candidate for retry', async () => {
  const build = process.env.TCW_BUILD_DIR ? path.resolve(process.env.TCW_BUILD_DIR) : path.join(__dirname, '../build');
  const M = await require(path.join(build, 'terrax_world_wasm_web.js'))({ wasmBinary: fs.readFileSync(path.join(build, 'terrax_world_wasm_web.wasm')) });
  const pointers = [];
  const alloc = n => { const p = M._tx_malloc(n); assert.ok(p); pointers.push(p); return p; };
  const hp = alloc(4), ep = alloc(48), sp = alloc(96), input = alloc(1048576);
  const bytes = makeCircuitWorld([{ x: 2, y: 2, type: 1, wires: 1 }], 8, 8);
  function ready(source) {
    assert.equal(M._terra_world_stream_open_begin(source, bytes.length, hp), 0);
    const task = M.HEAPU32[hp / 4];
    for (let n = 0; n < 10000; n++) {
      assert.equal(M._terra_world_stream_step(task, 64, ep), 0);
      const e = Array.from(M.HEAPU32.subarray(ep / 4, ep / 4 + 12));
      if (e[1] === 4) return task;
      if (e[1] === 0) continue;
      assert.equal(e[1], 1); assert.equal(e[2], source);
      const part = bytes.subarray(e[3], e[3] + e[4]); assert.equal(part.length, e[4]);
      M.HEAPU8.set(part, input);
      assert.equal(M._terra_world_stream_supply_source(task, source, e[3], input, part.length), 0);
    }
    assert.fail('candidate did not become READY');
  }
  const initial = ready(1);
  assert.equal(M._terra_world_stream_adopt(initial, 1, hp), 0);
  let world = M.HEAPU32[hp / 4];
  assert.equal(M._terra_world_stream_close(initial), 0);
  assert.equal(M._terra_circuit_world_begin(world, 2, 0, 0, 8 * 1024 * 1024, hp), 0);
  const circuit = M.HEAPU32[hp / 4];
  assert.equal(M._terra_world_close(world), 8);
  assert.equal(M._terra_world_workspace_checkpoint_size(world, hp), 8);
  assert.equal(M.HEAPU32[hp / 4], 0);
  const pinnedHeap = M._tx_native_heap_used();
  assert.equal(M._txw_begin_pixel_art_indexed(world, 0, 0, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0), -1);
  assert.equal(M._txw_add_pixel_art_chunks_bulk_fast(world, 0, 0, 0), -1);
  assert.equal(M._tx_native_heap_used(), pinnedHeap);
  const replacement = ready(3);
  assert.equal(M._terra_circuit_world_stats(circuit, sp), 0);
  const before = Buffer.from(M.HEAPU8.subarray(sp, sp + 96));
  const heap = M._tx_native_heap_used();
  for (let n = 0; n < 3; n++) {
    M.HEAPU32[hp / 4] = 99;
    assert.equal(M._terra_world_stream_adopt(replacement, 3, hp), -1);
    assert.equal(M.HEAPU32[hp / 4], 0);
    assert.equal(M._terra_circuit_world_stats(circuit, sp), 0);
    assert.deepEqual(Buffer.from(M.HEAPU8.subarray(sp, sp + 96)), before);
    assert.equal(M._tx_native_heap_used(), heap);
    assert.equal(M._terra_world_stream_step(replacement, 1, ep), 0);
    assert.equal(M.HEAPU32[ep / 4 + 1], 4, 'candidate remains READY');
  }
  assert.equal(M._terra_circuit_world_cancel(circuit), 0);
  assert.equal(M._terra_world_stream_adopt(replacement, 3, hp), -1, 'cancel does not release the source lease');
  assert.equal(M._terra_circuit_world_close(circuit), 0);
  assert.equal(M._terra_world_stream_adopt(replacement, 3, hp), 0, 'same candidate can be adopted after confirmed retirement');
  const old = world; world = M.HEAPU32[hp / 4]; assert.notEqual(world, old);
  assert.equal(M._terra_world_stream_close(replacement), 0);
  assert.equal(M._terra_world_workspace_begin(world, 8 * 1024 * 1024, hp), 0);
  const token = M.HEAPU32[hp / 4], transactionHeap = M._tx_native_heap_used();
  assert.equal(M._terra_circuit_world_begin(world, 2, 0, 0, 8 * 1024 * 1024, hp), -3);
  assert.equal(M.HEAPU32[hp / 4], 0); assert.equal(M._tx_native_heap_used(), transactionHeap);
  assert.equal(M._terra_world_workspace_rollback(world, token), 0);
  assert.equal(M._terra_world_close(world), 0);
  for (const p of pointers) M._tx_free(p);
  assert.equal(M._tx_native_heap_used(), 0); assert.equal(M._tx_bridge_heap_used(), 0);
});
