'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { makeCircuitWorld, makeDriver } = require('../helpers/circuit-world.js');

// Wiring.PixelBoxPass at 8255d34616c780af12079425ac92a0a7aed87d71:
// only horizontal + vertical signals in one TripWire toggle tile 445.
function fixture(on = false) {
  const cells = [];
  for (let x = 2; x < 10; x++) cells.push({ x, y: 10, wires: 1 });
  for (let y = 2; y < 10; y++) cells.push({ x: 2, y, wires: 1 });
  for (let x = 3; x <= 10; x++) cells.push({ x, y: 2, wires: 2 });
  for (let y = 3; y < 10; y++) cells.push({ x: 10, y, wires: 2 });
  cells.push({ x: 10, y: 10, type: 445, fx: on ? 18 : 0, wires: 3 });
  return makeCircuitWorld(cells);
}
const pulse = (d, axes, count = 1) => d.command(2, { x: axes === 2 ? 3 : 2, y: 2, width: axes === 3 ? 2 : 1, mask: axes, count });
const pixel = d => d.cell(10, 10).fx;

test('current PixelBox leaves both on and off frames unchanged under single-axis signals', async () => {
  const d = await makeDriver();
  try {
    for (const on of [false, true]) {
      assert.equal(d.open(fixture(on)).event[11], 0);
      for (const axes of [1, 1, 2, 2]) { pulse(d, axes, 3); assert.equal(pixel(d), on ? 18 : 0); }
    }
  } finally { d.dispose(); }
});

test('current PixelBox toggles on every simultaneous crossed TripWire, including repeated pulses', async () => {
  const d = await makeDriver();
  try {
    d.open(fixture());
    pulse(d, 3); assert.equal(pixel(d), 18);
    pulse(d, 3); assert.equal(pixel(d), 0);
    pulse(d, 3, 3); assert.equal(pixel(d), 18);
    pulse(d, 3, 2); assert.equal(pixel(d), 18);
    pulse(d, 3); assert.equal(pixel(d), 0);
  } finally { d.dispose(); }
});

test('current PixelBox does not pair horizontal and vertical signals from different TripWires', async () => {
  const d = await makeDriver();
  try {
    for (const on of [false, true]) {
      d.open(fixture(on));
      for (const axes of [1, 2, 2, 1]) { pulse(d, axes); assert.equal(pixel(d), on ? 18 : 0); }
      pulse(d, 3); assert.equal(pixel(d), on ? 0 : 18);
    }
  } finally { d.dispose(); }
});

test('only the declared command set is accepted and no selectable PixelBox rule is advertised', async () => {
  const d = await makeDriver();
  try {
    const start = d.M._terra_build_info_json(), end = d.M.HEAPU8.indexOf(0, start);
    assert.ok(start > 0 && end > start);
    const identity = JSON.parse(Buffer.from(d.M.HEAPU8.subarray(start, end)).toString('utf8'));
    assert.equal(Object.hasOwn(identity, 'circuitWorldPixelBoxModes'), false);
    assert.equal(identity.circuitWorldStreamingTwld, false);
    d.open(fixture(true));
    const before = d.cell(10, 10);
    assert.equal(d.start(9, { mask: 1 }), -1);
    assert.deepEqual(d.cell(10, 10), before);
    assert.equal(d.event()[11], 0);
    pulse(d, 3); assert.equal(pixel(d), 0);
  } finally { d.dispose(); }
});

test('cancelling a repeated crossed pulse restores a completed PixelBox transition', async () => {
  const d = await makeDriver();
  try {
    d.open(fixture());
    const before = d.stats();
    assert.equal(d.start(2, { x: 2, y: 2, width: 2, mask: 3, count: 10000 }), 0);
    for (let n = 0; n < 1000 && d.stats()[20] < before[20] + 3; n++) {
      const e = d.event(false, 1); if (e[1] >= 1 && e[1] <= 3) d.respond(e);
    }
    assert.ok(d.stats()[20] >= before[20] + 3,
      'both nets and PixelBoxPass of the first pulse completed before the next pulse began');
    d.cancel(); assert.equal(pixel(d), 0);
    assert.deepEqual(d.stats().slice(18, 20), before.slice(18, 20), 'cancellation restores the clock');
    pulse(d, 3); assert.equal(pixel(d), 18);
  } finally { d.dispose(); }
});
