'use strict';
// Run from repo root: node TerraWasm/bench/pixel_art_8400x2400.js [solid|varied] [palette-count]
// Measures actual viewer chunk upload + native commit, excluding canvas creation,
// mapping resolution, previews, and filesystem persistence. Optional MAX_MS gate.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const {performance} = require('node:perf_hooks');
const {getPrimaryWorldPath} = require('../tests/helpers/fixtures');
const {minimalTxci} = require('../tests/helpers/minimal-txci');
const runtime = require('../build/terrax_world_wasm.js');
const WIDTH = 8400, HEIGHT = 2400, COLS = Math.ceil(WIDTH / 64);

async function main() {
  const pattern = process.argv[2] || 'varied';
  assert.ok(['solid', 'varied'].includes(pattern));
  const count = Number(process.argv[3] || 256);
  assert.ok(Number.isInteger(count) && count >= 2 && count <= 65535);
  const palette = ['transparent'], overrides = [];
  for (let i = 1; i <= count; i++) {
    palette.push('#' + i.toString(16).padStart(6, '0'));
    overrides.push({r: i >> 16, g: (i >> 8) & 255, b: i & 255, a: 255,
      tileType: 1, wallType: 1, tileColor: i % 31, wallColor: (i >> 5) % 31, activeMode: 4});
  }
  const chunks = new Map();
  for (let cy = 0; cy < Math.ceil(HEIGHT / 64); cy++) for (let cx = 0; cx < COLS; cx++) {
    const buf = new Uint16Array(4096); let used = 0;
    for (let y = 0; y < 64 && cy * 64 + y < HEIGHT; y++)
      for (let x = 0; x < 64 && cx * 64 + x < WIDTH; x++) {
        buf[y * 64 + x] = pattern === 'solid' ? 1 : 1 + ((cx * 64 + x) * 73 + (cy * 64 + y) * 151) % count;
        used++;
      }
    chunks.set(cy * COLS + cx, {buf, used});
  }
  assert.equal([...chunks.values()].reduce((n, c) => n + c.used, 0), WIDTH * HEIGHT);
  const source = fs.readFileSync(path.resolve(__dirname, '../../viewer-app/features/world-write/services/indexed-pixel-write.js'), 'utf8');
  const {applyIndexedPixelArtBatched} = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
  const M = await runtime();
  let beginMs = 0, nativeUploadMs = 0;
  const instrumented = Object.create(M);
  instrumented._txw_begin_pixel_art_indexed = (...args) => {
    const t = performance.now(), status = M._txw_begin_pixel_art_indexed(...args);
    beginMs += performance.now() - t; return status;
  };
  instrumented._txw_add_pixel_art_chunks_bulk_fast = (...args) => {
    const t = performance.now(), status = M._txw_add_pixel_art_chunks_bulk_fast(...args);
    nativeUploadMs += performance.now() - t; return status;
  };
  const alloc = size => {const p = M._tx_malloc(size); assert.ok(p, `allocation failed: ${size}`); return p;};
  const bytes = fs.readFileSync(getPrimaryWorldPath()), input = alloc(bytes.length), hp = alloc(4), required = alloc(4);
  M.HEAPU8.set(bytes, input);
  assert.equal(M._terra_world_open_from_buffer(input, bytes.length, hp), 0);
  let handle = M.HEAPU32[hp >>> 2]; M._tx_free(input);
  // Header dimension check prevents a full canvas from silently being clipped.
  const name = alloc(7), size = alloc(8); M.stringToUTF8('header', name, 7);
  assert.equal(M._terra_section_get_json(handle, name, 0, 0n, size), 0);
  const headerPtr = alloc(M.HEAPU32[size >>> 2]);
  assert.equal(M._terra_section_get_json(handle, name, headerPtr, BigInt(M.HEAPU32[size >>> 2]), size), 0);
  const header = JSON.parse(M.UTF8ToString(headerPtr));
  assert.equal(header.maxTilesX, WIDTH); assert.equal(header.maxTilesY, HEIGHT);
  [headerPtr, size, name].forEach(M._tx_free);
  const started = performance.now();
  assert.equal(await applyIndexedPixelArtBatched({Module: instrumented, malloc: alloc, free: M._tx_free}, {handle}, {
    width: WIDTH, height: HEIGHT, palette, overrides, chunks, txciGz: minimalTxci(),
    ...(process.env.NO_UI_YIELD ? {yieldTask: async () => {}} : {}),
  }), chunks.size);
  const uploaded = performance.now();
  assert.equal(M._terra_world_commit_to_buffer(handle, 0, 0, required, hp), 0);
  const rebuilt = performance.now(), length = M.HEAPU32[required >>> 2], output = alloc(length);
  assert.equal(M._terra_world_commit_to_buffer(handle, output, length, required, hp), 0);
  handle = M.HEAPU32[hp >>> 2]; assert.ok(handle);
  const reopened = performance.now();
  const result = Buffer.from(M.HEAPU8.subarray(output, output + length));
  const copied = performance.now();
  assert.equal(result.readUInt32LE(0), bytes.readUInt32LE(0));
  if (pattern === 'varied') assert.ok(length > WIDTH * HEIGHT * 4, 'varied pixels must defeat solid-color RLE');
  const report = {pattern, paletteColors: count, dimensions: `${WIDTH}x${HEIGHT}`, cpu: os.cpus()[0].model,
    node: process.version, uiYield: !process.env.NO_UI_YIELD, beginMs, nativeUploadMs, uploadMs: uploaded-started, rebuildMs: rebuilt-uploaded,
    serializeReopenMs: reopened-rebuilt, copyToJsMs: copied-reopened, totalMs: copied-started,
    outputBytes: length, wasmLinearBytes: M.HEAPU8.length, nativePeakBytes: M._tx_native_heap_peak(),
    bridgePeakBytes: M._tx_bridge_heap_peak(), rssBytes: process.memoryUsage().rss};
  for (const k of Object.keys(report)) if (k.endsWith('Ms')) report[k] = Math.round(report[k] * 10) / 10;
  console.log(JSON.stringify(report, null, 2));
  M._tx_free(output); M._tx_free(required); M._tx_free(hp); assert.equal(M._terra_world_close(handle), 0);
  assert.equal(M._tx_native_heap_used(), 0); assert.equal(M._tx_bridge_heap_used(), 0);
  if (process.env.MAX_MS) assert.ok(report.totalMs <= Number(process.env.MAX_MS), `write exceeded ${process.env.MAX_MS}ms`);
}
main().catch(e => {console.error(e); process.exitCode = 1;});
