'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const zlib = require('node:zlib');
const { makeSectionedWorld } = require('./sectioned-world');

// Independent, small sectioned WLD encoder for mechanism contracts. It uses
// WorldFile's published flags and never asks the DUT to construct its fixture.
function makeCircuitWorld(cells, width = 40, height = 32, version = 196) {
  const name = 'circuit-contract', base = makeSectionedWorld(version, { worldName: name });
  const count = base.readUInt16LE(24), oldStart = base.readUInt32LE(26);
  const header = Buffer.from(base.subarray(oldStart));
  const dimensions = Buffer.alloc(8); dimensions.writeInt32LE(500); dimensions.writeInt32LE(1000, 4);
  const at = header.indexOf(dimensions); assert.ok(at >= 0);
  header.writeInt32LE(height, at); header.writeInt32LE(width, at + 4);
  const framed = new Set([4, 10, 21, 33, 49, 55, 85, 88, 132, 135, 136, 138, 144, 235, 314, 378, 395, 411, 419, 420, 423, 424, 425, 445, 467, 470, 471, 475, 520, 573, 597, 698, 723, 724]);
  // Include all pinned 1.4.5.8 entity tile IDs so rejection tests exercise the
  // section-data guard, rather than accidentally failing a type-count bound.
  const tileCount = 754, mask = Buffer.alloc(Math.ceil(tileCount / 8)); for (const type of framed) mask[type >>> 3] |= 1 << (type & 7);
  const important = Buffer.alloc(2); important.writeUInt16LE(tileCount);
  const format = Buffer.concat([base.subarray(0, oldStart - 3), important, mask]);
  const records = [], map = new Map(cells.map(cell => [`${cell.x},${cell.y}`, cell]));
  for (let x = 0; x < width; x++) for (let y = 0; y < height; y++) {
    const t = map.get(`${x},${y}`) || {}, out = [], active = t.type !== undefined, wires = t.wires || 0;
    let f1 = active ? 2 : 0, f2 = (wires & 7) << 1, f3 = (wires & 8) ? 32 : 0;
    if (active && t.type > 255) f1 |= 32;
    if (t.actuator) f3 |= 2; if (t.inactive) f3 |= 4;
    if (t.wall) f1 |= 4; if ((t.wall || 0) > 255) f3 |= 64;
    if (t.paint) f3 |= 8; if (t.wallPaint) f3 |= 16;
    if (f3) f2 |= 1; if (f2) f1 |= 1;
    out.push(f1); if (f1 & 1) out.push(f2); if (f2 & 1) out.push(f3);
    if (active) {
      out.push(t.type & 255); if (f1 & 32) out.push(t.type >>> 8);
      if (framed.has(t.type)) out.push((t.fx || 0) & 255, ((t.fx || 0) >> 8) & 255, (t.fy || 0) & 255, ((t.fy || 0) >> 8) & 255);
      if (t.paint) out.push(t.paint);
    }
    if (t.wall) { out.push(t.wall & 255); if (t.wallPaint) out.push(t.wallPaint); }
    if (f3 & 64) out.push(t.wall >>> 8);
    records.push(Buffer.from(out));
  }
  const footer = Buffer.concat([Buffer.from([1, Buffer.byteLength(name)]), Buffer.from(name), Buffer.from([1, 0, 0, 0])]);
  const sections = version === 326
    ? [header, Buffer.concat(records), Buffer.alloc(2), Buffer.alloc(2), Buffer.alloc(6), Buffer.alloc(4), Buffer.alloc(4), Buffer.alloc(4), Buffer.alloc(12), Buffer.alloc(1), footer]
    : [header, Buffer.concat(records), Buffer.from([0, 0, 40, 0]), Buffer.alloc(2), Buffer.alloc(2), Buffer.alloc(4), Buffer.alloc(4), Buffer.alloc(4), footer];
  assert.equal(sections.length, count);
  let offset = format.length;
  sections.forEach((s, i) => { format.writeUInt32LE(offset, 26 + i * 4); offset += s.length; });
  return Buffer.concat([format, ...sections]);
}

// One named ColorPixelBox plus incompressible unknown metadata keeps a save
// replay spread over several input chunks, so cancellation can occur mid-gzip.
function makeCircuitTwld(width = 40, height = 32) {
  const u32 = n => { const b = Buffer.alloc(4); b.writeUInt32BE(n); return b; };
  const str = s => { const b = Buffer.from(s), n = Buffer.alloc(2); n.writeUInt16BE(b.length); return Buffer.concat([n, b]); };
  const tag = (type, name, bytes) => Buffer.concat([Buffer.from([type]), str(name), bytes]);
  const pixels = [];
  for (let x = 0; x < width; x++) for (let y = 0; y < height; y++) pixels.push(x === 10 && y === 10 ? Buffer.from([0xbb, 2, 0, 18, 0, 36, 0]) : Buffer.alloc(2));
  const data = Buffer.concat(pixels), unknown = Buffer.alloc(16384); let state = 0x1729abcd;
  for (let i = 0; i < unknown.length; i++) { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; unknown[i] = state >>> 24; }
  const entry = Buffer.concat([tag(3, 'value', u32(699)), tag(8, 'mod', str('WireHead')), tag(8, 'name', str('ColorPixelBox')), tag(1, 'framed', Buffer.from([1])), Buffer.from([0])]);
  const tiles = Buffer.concat([tag(7, 'tileData', Buffer.concat([u32(data.length), data])), tag(9, 'tileMap', Buffer.concat([Buffer.from([10]), u32(1), entry])), Buffer.from([0])]);
  return zlib.gzipSync(tag(10, '', Buffer.concat([tag(10, 'tiles', tiles), tag(7, 'future-unknown-blob', Buffer.concat([u32(unknown.length), unknown])), Buffer.from([0])])));
}

async function makeDriver() {
  const M = await require('../../build/terrax_world_wasm_web.js')({ wasmBinary: fs.readFileSync(require.resolve('../../build/terrax_world_wasm_web.wasm')) });
  const pointers = [], alloc = bytes => { const p = M._tx_malloc(bytes); assert.ok(p); pointers.push(p); return p; };
  const hp = alloc(4), ep = alloc(48), cp = alloc(64), sp = alloc(96), input = alloc(1048576), data = alloc(1048576);
  const sources = new Map(); let world = 0, circuit = 0, openTask = 0, progress = 0;
  const okay = status => assert.ok(status === 0 || status === 1, `native status ${status}`);
  function event(opening = false, work = 1024) {
    okay(opening ? M._terra_world_stream_step(openTask, work, ep) : M._terra_circuit_world_step(circuit, work, ep));
    const e = Array.from(M.HEAPU32.subarray(ep >>> 2, (ep >>> 2) + 12)); assert.equal(e[0], 1); return e;
  }
  function pump(opening = false) {
    const rows = [];
    for (let steps = 0; steps < 1000000; steps++) {
      const e = event(opening); progress++;
      if (e[1] >= 1 && e[1] <= 3) rows.push(...respond(e, opening));
      else if (e[1] === 4) return { rows, event: e };
      else assert.equal(e[1], 0);
    }
    assert.fail('circuit did not finish bounded execution');
  }
  function respond(e, opening = false) {
    const rows = [];
    if (e[1] === 1) {
      assert.ok(e[4] > 0 && e[4] <= 1048576);
      const bytes = sources.get(e[2]).subarray(e[3], e[3] + e[4]); assert.equal(bytes.length, e[4]);
      M.HEAPU8.set(bytes, input); okay(opening ? M._terra_world_stream_supply_source(openTask, e[2], e[3], input, bytes.length) : M._terra_circuit_world_supply(circuit, e[2], e[3], input, bytes.length));
    } else if (e[1] === 2 && !opening) {
      assert.ok(e[4] <= 1048576);
      const old = sources.get(e[2]) || Buffer.alloc(0); assert.ok(e[3] <= old.length, 'compiler writes sequentially or overwrites existing scratch');
      const bytes = Buffer.alloc(Math.max(old.length, e[3] + e[4])); old.copy(bytes); Buffer.from(M.HEAPU8.subarray(e[5], e[5] + e[4])).copy(bytes, e[3]); sources.set(e[2], bytes);
      okay(M._terra_circuit_world_ack(circuit));
    } else if (e[1] === 3 && !opening) {
      const words = e[9] === 7 || e[9] === 8 ? 8 : 4;
      assert.equal(e[4], e[10] * words * 4);
      const values = M.HEAPU32.subarray(e[5] >>> 2, (e[5] >>> 2) + e[10] * words);
      for (let i = 0; i < values.length; i += words) rows.push(Array.from(values.subarray(i, i + words)));
      okay(M._terra_circuit_world_ack(circuit));
    } else assert.fail('event does not need a host response');
    return rows;
  }
  function start(kind, o = {}) {
    const records = o.rawRecords || o.records || [];
    for (let i = 0; i < records.length; i++) M.HEAPU32.set(o.rawRecords ? records[i] : [records[i].x, records[i].y, records[i].value || 0, 0], (data >>> 2) + i * 4);
    M.HEAPU32.set([1, kind, o.x || 0, o.y || 0, o.width || 1, o.height || 1, o.stride || 1, o.mask || 15, o.count || 0, records.length ? data : 0, records.length, o.source || 0, o.flags || 0, o.auxSource || 0, 0, 0], cp >>> 2);
    return M._terra_circuit_world_command(circuit, cp);
  }
  const command = (kind, options) => { okay(start(kind, options)); return pump(); };
  const stats = () => { okay(M._terra_circuit_world_stats(circuit, sp)); return Array.from(M.HEAPU32.subarray(sp >>> 2, (sp >>> 2) + 24)); };
  const close = () => {
    if (circuit) { okay(M._terra_circuit_world_close(circuit)); circuit = 0; }
    if (world) { okay(M._terra_world_close(world)); world = 0; }
    if (openTask) { okay(M._terra_world_stream_close(openTask)); openTask = 0; }
    assert.equal(M._tx_native_heap_used(), 0, 'all native session state released');
  };
  function open(bytes, maximum = 48 * 1024 * 1024, sidecar) {
    close(); sources.clear(); sources.set(1, bytes); sources.set(2, Buffer.alloc(0)); if (sidecar) sources.set(3, sidecar);
    okay(M._terra_world_stream_open_begin(1, bytes.length, hp)); openTask = M.HEAPU32[hp >>> 2]; pump(true);
    okay(M._terra_world_stream_adopt(openTask, 1, hp)); world = M.HEAPU32[hp >>> 2]; okay(M._terra_world_stream_close(openTask)); openTask = 0;
    okay(M._terra_circuit_world_begin(world, 2, sidecar ? 3 : 0, sidecar?.length || 0, maximum, hp)); circuit = M.HEAPU32[hp >>> 2]; return pump();
  }
  return { M, open, close, command, start, event, respond, pump, stats, sources, get worldHandle() { return world; },
    cancel() { okay(M._terra_circuit_world_cancel(circuit)); },
    cell(x, y) { const row = command(1, { x, y }).rows[0]; return { x: row[0], y: row[1], type: row[2] & 65535, flags: row[2] >>> 16 & 255, wires: row[2] >>> 24 & 15, fx: row[3] & 65535, fy: row[3] >>> 16 }; },
    dispose() { close(); for (const p of pointers) M._tx_free(p); },
  };
}
module.exports = { makeCircuitWorld, makeCircuitTwld, makeDriver };
