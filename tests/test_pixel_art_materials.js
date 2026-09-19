'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { getPrimaryWorldPath } = require('./helpers/fixtures');
const createWasm = require('../build/terrax_world_wasm.js');

// Read the saved WLD bytes independently of the engine's tile decoder.
function firstRow(bytes, height, count) {
  const pointerStart = bytes.readUInt32LE(0) >= 135 ? 26 : 6;
  const pointers = bytes.readUInt16LE(pointerStart - 2);
  const tileCountOffset = pointerStart + pointers * 4;
  const important = bytes.subarray(tileCountOffset + 2);
  let offset = bytes.readUInt32LE(pointerStart + 4);
  const u8 = () => bytes[offset++];
  const u16 = () => { const n = bytes.readUInt16LE(offset); offset += 2; return n; };
  const result = [];
  for (let x = 0; x < count; x++) {
    for (let y = 0; y < height;) {
      const f1 = u8(), f2 = f1 & 1 ? u8() : 0, f3 = f2 & 1 ? u8() : 0;
      if (f3 & 1) u8();
      const active = Boolean(f1 & 2);
      let type = 0, wall = 0, tileColor = 0, wallColor = 0;
      if (active) {
        type = f1 & 32 ? u16() : u8();
        if (important[type >> 3] & (1 << (type & 7))) offset += 4;
        if (f3 & 8) tileColor = u8();
      }
      if (f1 & 4) { wall = u8(); if (f3 & 16) wallColor = u8(); }
      if (f1 & 24) u8();
      if (f3 & 64) wall |= u8() << 8;
      const rle = f1 >> 6;
      const repeat = rle === 1 ? u8() : rle ? u16() : 0;
      if (y === 0) result.push({ active, type, wall, tileColor, wallColor });
      y += repeat + 1;
      assert.ok(offset <= bytes.length && y <= height);
    }
  }
  return result;
}

test('combined tile/wall, either empty, both empty, dirt zero and paints survive WLD save and reopen', async () => {
  const M = await createWasm();
  const allocations = [];
  const alloc = data => { const p = M._tx_malloc(typeof data === 'number' ? data : data.length); assert.ok(p); allocations.push(p); if (typeof data !== 'number') M.HEAPU8.set(data, p); return p; };
  const str = value => alloc(Buffer.from(value + '\0'));
  const hp = alloc(4);
  assert.equal(M._terra_world_open(str(getPrimaryWorldPath()), hp), 0);
  let handle = M.HEAPU32[hp >> 2];
  const output = path.resolve(__dirname, '../build/material-roundtrip.wld');
  try {
    const size = alloc(8), name = str('header');
    assert.equal(M._terra_section_get_json(handle, name, 0, 0n, size), 0);
    const length = M.HEAPU32[size >> 2], buffer = alloc(length);
    assert.equal(M._terra_section_get_json(handle, name, buffer, BigInt(length), size), 0);
    const height = JSON.parse(M.UTF8ToString(buffer)).maxTilesY;
    const cases = [
      { mode: 4, tile: 1, wall: 7, tileColor: 2, wallColor: 3 },
      { mode: 4, tile: 0, wall: 7 },
      { mode: 4, tile: 1, wall: 0 },
      { mode: 2, tile: 0, wall: 7 },
      { mode: 0, tile: 0, wall: 0 },
    ];
    const palette = Buffer.alloc((cases.length + 1) * 4);
    const maps = Buffer.alloc(cases.length * 12);
    const indices = new Uint16Array(4096);
    cases.forEach((entry, i) => {
      palette.set([i + 1, 2, 3, 255], (i + 1) * 4);
      const at = i * 12;
      maps.set([i + 1, 2, 3, 255], at);
      maps.writeUInt16LE(entry.tile, at + 4); maps.writeUInt16LE(entry.wall, at + 6);
      maps[at + 8] = entry.tileColor || 0; maps[at + 9] = entry.wallColor || 0; maps[at + 10] = entry.mode;
      indices[i] = i + 1;
    });
    const txci = fs.readFileSync(path.resolve(__dirname, '../data/terraria_color_index.txci'));
    assert.equal(M._txw_begin_pixel_art_indexed(handle, 0, 0, cases.length, 1, alloc(palette), cases.length + 1,
      alloc(txci), txci.length, 0, 0, alloc(maps), cases.length, 0), 0);
    assert.equal(M._txw_add_pixel_art_chunk(handle, 0, 0, alloc(new Uint8Array(indices.buffer)), indices.length, cases.length), 0);
    assert.equal(M._terra_world_save(handle, str(output)), 0);
    const expected = cases.map(entry => ({ active: entry.mode === 4, type: entry.mode === 4 ? entry.tile : 0,
      wall: entry.wall, tileColor: entry.tileColor || 0, wallColor: entry.wallColor || 0 }));
    assert.deepEqual(firstRow(fs.readFileSync(output), height, cases.length), expected);
    M._terra_world_close(handle); handle = 0;
    assert.equal(M._terra_world_open(str(output), hp), 0); handle = M.HEAPU32[hp >> 2];
    const reopened = output + '.again';
    assert.equal(M._terra_world_save(handle, str(reopened)), 0);
    assert.deepEqual(firstRow(fs.readFileSync(reopened), height, cases.length), expected);
  } finally {
    if (handle) M._terra_world_close(handle);
    allocations.forEach(p => M._tx_free(p));
  }
});
