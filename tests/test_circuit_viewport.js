'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { makeCircuitWorld, makeCircuitTwld, makeDriver } = require('./helpers/circuit-world');

function fixture(rle) {
  const cells = new Map();
  const put = (x, y, patch) => cells.set(`${x},${y}`, { ...cells.get(`${x},${y}`), x, y, ...patch });
  for (let x = 0; x < 196; x++) for (let y = 60; y < 132; y++) {
    put(x, y, { wall: 300, wallPaint: 26, invisibleWall: y < 84, fullbrightWall: y >= 108 });
  }
  for (let x = 20; x <= 170; x++) put(x, 70, { wires: 1 });
  for (let y = 2; y < 500; y++) put(64, y, { wires: y === 70 ? 3 : 2 });
  for (let y = 3; y < 490; y++) put(130, y, { wires: y === 70 ? 9 : 8 });
  put(64, 70, { type: 424, fx: 0 });
  put(20, 70, { type: 136 });
  put(30, 70, { type: 144, fx: 18 });
  put(80, 70, { type: 1, actuator: true, paint: 7, invisibleBlock: true });
  put(100, 70, { type: 419 });
  put(100, 71, { type: 420, wires: 4 });
  put(110, 68, { type: 419, fx: 36, wires: 1 });
  put(110, 69, { type: 419, fx: 18, wires: 8 });
  put(110, 70, { type: 420, fx: 36, wires: 2 });
  put(150, 70, { type: 33 });
  return { cells, bytes: makeCircuitWorld([...cells.values()], 196, 512, 326, { rle }) };
}

function command(d, kind, options, quantum = 4096) {
  assert.ok([0, 1].includes(d.start(kind, options)));
  const rows = []; let steps = 0, reads = 0, sourceBytes = 0;
  for (; steps < 1000000; steps++) {
    const event = d.event(false, quantum);
    if (event[1] === 4) return { rows, steps: steps + 1, reads, sourceBytes };
    if (event[1] === 1) { reads++; sourceBytes += event[4]; }
    if (event[1] >= 1 && event[1] <= 3) rows.push(...d.respond(event));
  }
  assert.fail('viewport did not complete its bounded command');
}

function expectedWalls(cells, request) {
  const out = [], stride = request.stride || 1;
  for (let x = request.x; x < request.x + request.width; x += stride) {
    for (let y = request.y; y < request.y + request.height; y += stride) {
      const t = cells.get(`${x},${y}`) || {};
      const flags = (t.wall ? 1 : 0) | 32 | (t.invisibleWall ? 64 : 0) | (t.fullbrightWall ? 128 : 0);
      out.push([x, y, (t.wall || 0) | flags << 16, t.wallPaint || 0]);
    }
  }
  return out;
}

test('indexed wall viewports preserve paint/coatings and sampling across RLE, column checkpoints, and yields', async () => {
  const f = fixture(true), d = await makeDriver();
  try {
    d.open(f.bytes);
    const cases = [
      { x: 62, y: 58, width: 6, height: 78, stride: 1 },
      { x: 125, y: 75, width: 14, height: 44, stride: 3 },
      { x: 195, y: 510, width: 1, height: 2, stride: 1 },
      { x: 32, y: 72, width: 3, height: 5, stride: 0xffffffff },
    ];
    for (const request of cases) {
      const result = command(d, 1, { ...request, flags: 2 }, 1);
      assert.deepEqual(result.rows, expectedWalls(f.cells, request));
      assert.deepEqual(command(d, 1, { ...request, flags: 2 }, 4096).rows, result.rows);
    }
    const request = cases[0];
    assert.equal(d.start(1, { ...request, flags: 2 }), 0);
    d.event(false, 1); d.cancel();
    assert.deepEqual(command(d, 1, { ...request, flags: 2 }, 1).rows, expectedWalls(f.cells, request));
  } finally { d.dispose(); }
});

test('compressed and uncompressed wiring retain the same live frames after pulses, clocks, writes and cancellation', async () => {
  const d = await makeDriver(), snapshots = [];
  const request = { x: 18, y: 64, width: 155, height: 12, stride: 1 };
  try {
    for (const rle of [false, true]) {
      const f = fixture(rle); d.open(f.bytes);
      const rows = [command(d, 1, request, 1).rows];
      const before = d.cell(100, 70);
      command(d, 2, { x: 100, y: 70, width: 1, height: 1, mask: 1 }, 1);
      // A directly seeded ordinary lamp is skipped by TripWire.
      assert.deepEqual(d.cell(100, 70), before);
      command(d, 2, { x: 20, y: 70, width: 1, height: 1, mask: 1 }, 1);
      assert.equal(d.cell(100, 70).fx, 18, 'the connected ordinary lamp toggles');
      rows.push(command(d, 1, request, 17).rows);
      command(d, 5, { records: [{ x: 100, y: 70, value: 0 }] }, 1);
      assert.equal(d.cell(100, 70).fx, 0, 'lamp writes are visible through warm source bytes');
      rows.push(command(d, 1, request).rows);
      command(d, 2, { x: 30, y: 70, width: 1, height: 1, flags: 1 }, 1);
      assert.equal(d.cell(30, 70).fy, 18, 'the timer is enabled before advancing its scheduler');
      command(d, 3, { count: 180 }, 1);
      assert.equal(d.cell(100, 70).fx, 18, 'a timer pulse updates the next warm viewport');
      rows.push(command(d, 1, request).rows);
      assert.equal(d.start(2, { x: 20, y: 70, mask: 1, count: 1000 }), 0);
      d.event(false, 1); d.cancel();
      assert.deepEqual(command(d, 1, request).rows, rows[rows.length - 1], 'cancel keeps committed frames');
      rows.push(command(d, 1, { ...request, x: request.x + 1, stride: 3 }, 1).rows);
      rows.push(command(d, 1, { ...request, y: request.y + 1, flags: 2 }, 1).rows);
      snapshots.push({ stats: d.stats().slice(10, 14), rows });
    }
    assert.deepEqual(snapshots[1], snapshots[0], 'RLE skipping preserves networks, gates, ordering and every result word');
  } finally { d.dispose(); }
});

test('warm viewport source reuse adds no retained cache and stays coherent across layers, commands and TWLD save', async () => {
  const d = await makeDriver(), f = fixture(true);
  const request = { x: 62, y: 64, width: 14, height: 12, stride: 1 };
  try {
    d.open(f.bytes);
    const initial = command(d, 1, request).rows, active = d.stats()[16];
    for (let i = 0; i < 12; i++) {
      const result = command(d, 1, { ...request, x: request.x + i % 2, flags: i % 2 ? 2 : 0 }, 7);
      assert.equal(result.reads, 0, 'an immutable source window survives viewport and layer changes');
      assert.equal(d.stats()[16], active, 'no extra viewport cache allocation');
    }
    assert.deepEqual(command(d, 1, request).rows, initial);
    const sidecar = makeCircuitTwld();
    d.open(makeCircuitWorld([{ x: 10, y: 10, wall: 300, wallPaint: 7 }], 40, 32, 326, { rle: true }), 48 * 1024 * 1024, sidecar);
    const cell = d.cell(10, 10), walls = command(d, 1, { x: 9, y: 9, width: 4, height: 4, flags: 2 }).rows;
    d.command(6, { source: 4, auxSource: 5 });
    assert.deepEqual(d.cell(10, 10), cell, 'WLD input cannot alias the last TWLD read');
    assert.deepEqual(command(d, 1, { x: 9, y: 9, width: 4, height: 4, flags: 2 }).rows, walls);
  } finally { d.dispose(); }
});
