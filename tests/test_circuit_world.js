'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { makeCircuitWorld, makeCircuitTwld, makeDriver } = require('./helpers/circuit-world');
const line = (x1, x2, y, wires) => Array.from({ length: x2 - x1 + 1 }, (_, n) => ({ x: x1 + n, y, wires }));

test('streamed WLD ordinary gates implement six original truth tables and skip seed lamps', async () => {
  const d = await makeDriver();
  try {
    const truth = [n => n === 2, n => n > 0, n => n !== 2, n => n === 0, n => n === 1, n => n !== 1];
    for (let style = 0; style < truth.length; style++) {
      d.open(makeCircuitWorld([
        ...line(2, 9, 6, 2), ...line(2, 9, 7, 1), ...line(11, 13, 8, 4),
        { x: 10, y: 6, type: 419, wires: 2 }, { x: 10, y: 7, type: 419, wires: 1 },
        { x: 10, y: 8, type: 420, fx: truth[style](0) ? 18 : 0, fy: style * 18, wires: 4 },
        { x: 14, y: 8, type: 419, wires: 4 },
      ]));
      let state = 0, gate = truth[style](0), output = false;
      for (const next of [1, 3, 2, 0]) {
        const bit = state ^ next;
        d.command(2, { x: 2, y: bit === 1 ? 6 : 7, mask: bit === 1 ? 2 : 1 });
        state = next; const expected = truth[style]((state & 1) + ((state >>> 1) & 1));
        if (expected !== gate) output = !output; gate = expected;
        assert.equal(d.cell(10, 8).fx, expected ? 18 : 0, `style ${style}, inputs ${state}`);
        assert.equal(d.cell(14, 8).fx, output ? 18 : 0, 'only gate state transitions emit a pulse');
      }
      // TripWire first SkipWire's every seed. The initiating ordinary lamp is
      // neither toggled nor checked, although the rest of its network is hit.
      const before = d.cell(10, 6), frame = d.cell(10, 8).fx;
      d.command(2, { x: 10, y: 6, mask: 2 });
      assert.equal(d.cell(10, 6).fx, before.fx);
      assert.equal(d.cell(10, 8).fx, frame);
    }
  } finally { d.dispose(); }
});

test('faulty trigger seed is suppressed while data bias and later genuine inputs remain correct', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([
      ...line(2, 9, 5, 1), ...line(2, 9, 6, 2), ...line(11, 13, 7, 4),
      { x: 10, y: 5, type: 419, fx: 36, wires: 1 }, { x: 10, y: 6, type: 419, wires: 2 },
      { x: 10, y: 7, type: 420, fx: 36, wires: 4 }, { x: 14, y: 7, type: 419, wires: 4 },
    ]));
    d.command(2, { x: 2, y: 6, mask: 2 }); assert.equal(d.cell(10, 6).fx, 18);
    d.command(2, { x: 2, y: 5, mask: 1 }); assert.equal(d.cell(14, 7).fx, 18);
    const fired = d.stats()[22];
    d.command(2, { x: 10, y: 5, mask: 1 }); assert.equal(d.stats()[22], fired); assert.equal(d.cell(14, 7).fx, 18);
    d.command(2, { x: 10, y: 6, mask: 2 }); assert.equal(d.cell(10, 6).fx, 18);
    d.command(2, { x: 2, y: 5, mask: 1 }); assert.equal(d.cell(14, 7).fx, 0);
  } finally { d.dispose(); }
});

test('HitSwitch timers use 60/180/300/30/15 ticks and cancel restores a complete batch', async () => {
  const d = await makeDriver();
  try {
    for (const [style, interval] of [60, 180, 300, 30, 15].entries()) {
      d.open(makeCircuitWorld([{ x: 4, y: 4, type: 144, fx: style * 18, wires: 1 }, { x: 5, y: 4, wires: 1 }, { x: 6, y: 4, type: 419, wires: 1 }]));
      d.command(2, { x: 4, y: 4, flags: 1 });
      assert.equal(d.cell(4, 4).fy, 18); assert.equal(d.cell(6, 4).fx, 0, 'timer interaction has no immediate output');
      d.command(3, { count: interval - 1 }); assert.equal(d.cell(6, 4).fx, 0);
      d.command(3, { count: 1 }); assert.equal(d.cell(6, 4).fx, 18); assert.equal(d.cell(4, 4).fy, 18, 'timer skips itself when it pulses');
      const before = d.stats(), beforeLamp = d.cell(6, 4).fx;
      assert.equal(d.start(3, { count: interval * 10 }), 0);
      for (let n = 0; n < 1000 && d.stats()[20] === before[20]; n++) d.event(false, 1);
      assert.ok(d.stats()[20] > before[20], 'a pulse really executed inside the uncommitted tick batch');
      d.cancel(); assert.equal(d.stats()[18], before[18]); assert.equal(d.cell(6, 4).fx, beforeLamp); assert.equal(d.cell(4, 4).fy, 18);
      d.command(3, { count: interval }); assert.equal(d.cell(6, 4).fx, 0, 'restored timer phase emits at its original deadline');
      d.command(2, { x: 4, y: 4, flags: 1 }); d.command(3, { count: interval * 2 }); assert.equal(d.cell(6, 4).fx, 0);
    }
  } finally { d.dispose(); }
});

test('clock batch cancel restores committed lamps, and wall samples/save preserve source sections', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([{ x: 4, y: 4, type: 136, wires: 1 }, { x: 5, y: 4, wires: 1 }, { x: 6, y: 4, type: 419, wires: 1 }, { x: 8, y: 8, type: 1, paint: 7, wall: 257, wallPaint: 8 }]));
    d.command(2, { x: 4, y: 4, flags: 1 }); assert.equal(d.cell(6, 4).fx, 18);
    const before = d.stats(); assert.equal(d.start(2, { x: 4, y: 4, count: 100 }), 0);
    for (let n = 0; n < 1000 && d.stats()[20] < before[20] + 3; n++) d.event(false, 1);
    assert.ok(d.stats()[20] >= before[20] + 3, 'more than one independent pulse executes before cancel');
    d.cancel(); assert.equal(d.cell(6, 4).fx, 18); assert.equal(d.cell(4, 4).fy, 18);
    const wall = d.command(1, { x: 8, y: 8, flags: 2 }).rows[0]; assert.deepEqual(wall, [8, 8, 257 | (33 << 16), 8]);
    const saved = d.command(6, { source: 9 }); assert.equal(saved.event[10], d.sources.get(9).length); const bytes = d.sources.get(9);
    d.open(bytes); assert.equal(d.cell(6, 4).fx, 18); assert.equal(d.cell(4, 4).fy, 18);
    assert.deepEqual(d.command(1, { x: 8, y: 8, flags: 2 }).rows[0], wall);
  } finally { d.dispose(); }
});

test('a second faulty lamp above the standard pattern remains an independently triggerable source', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([
      ...line(2, 9, 4, 1), ...line(11, 13, 7, 8),
      { x: 10, y: 4, type: 419, fx: 36, wires: 1 }, { x: 10, y: 5, type: 419, fx: 36, wires: 2 },
      { x: 10, y: 6, type: 419, fx: 18, wires: 4 }, { x: 10, y: 7, type: 420, fx: 36, wires: 8 },
      { x: 14, y: 7, type: 419, wires: 8 },
    ]));
    d.command(2, { x: 2, y: 4, mask: 1 }); assert.equal(d.cell(14, 7).fx, 18);
    d.command(2, { x: 10, y: 4, mask: 1 }); assert.equal(d.cell(14, 7).fx, 18, 'even a higher faulty seed is skipped');
  } finally { d.dispose(); }
});

test('vanilla PixelBox pairs axes within each TripWire and direct junction seeds use direction zero', async () => {
  const d = await makeDriver();
  try {
    const path = [...line(2, 9, 10, 1), ...line(3, 10, 2, 2)];
    for (let y = 2; y < 10; y++) path.push({ x: 2, y, wires: 1 });
    for (let y = 3; y < 10; y++) path.push({ x: 10, y, wires: 2 });
    d.open(makeCircuitWorld([...path, { x: 10, y: 10, type: 445, wires: 3 }]));
    d.command(2, { x: 2, y: 2, mask: 1 }); d.command(2, { x: 3, y: 2, mask: 2 }); assert.equal(d.cell(10, 10).fx, 0);
    d.command(2, { x: 2, y: 2, width: 2, mask: 3 }); assert.equal(d.cell(10, 10).fx, 18);
    d.command(2, { x: 2, y: 2, width: 2, mask: 3 }); assert.equal(d.cell(10, 10).fx, 0);
    d.open(makeCircuitWorld([{ x: 10, y: 10, type: 424, wires: 1 }, { x: 11, y: 10, type: 419, wires: 1 }, { x: 10, y: 11, type: 419, wires: 1 }]));
    d.command(2, { x: 10, y: 10, mask: 1 }); assert.equal(d.cell(11, 10).fx, 0); assert.equal(d.cell(10, 11).fx, 18);
  } finally { d.dispose(); }
});

test('wire traversal never enters the two-cell margin, but a margin seed can emit inward', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([{ x: 1, y: 4, type: 419, wires: 1 }, { x: 2, y: 4, type: 419, wires: 1 }, { x: 3, y: 4, wires: 1 }]));
    d.command(2, { x: 3, y: 4, mask: 1 }); assert.equal(d.cell(1, 4).fx, 0); assert.equal(d.cell(2, 4).fx, 18);
    d.command(2, { x: 1, y: 4, mask: 1 }); assert.equal(d.cell(1, 4).fx, 0); assert.equal(d.cell(2, 4).fx, 0, 'inward target is a hit, not another skipped seed');
    d.open(makeCircuitWorld([{ x: 1, y: 4, type: 144, fx: 72, wires: 1 }, { x: 2, y: 4, type: 419, wires: 1 }]));
    d.command(2, { x: 1, y: 4, flags: 1 }); d.command(3, { count: 15 }); assert.equal(d.cell(2, 4).fx, 18);
  } finally { d.dispose(); }
});

test('actuators and active stone obey material/support eligibility and source SkipWire', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([
      ...line(2, 9, 4, 1), { x: 10, y: 4, type: 1, actuator: true, wires: 1 },
      { x: 11, y: 4, type: 419, actuator: true, wires: 1 },
      ...line(2, 9, 8, 1), { x: 10, y: 7, type: 21 }, { x: 10, y: 8, type: 1, actuator: true, wires: 1 },
      ...line(2, 9, 12, 1), { x: 10, y: 12, type: 131, wires: 1 },
      ...line(2, 9, 16, 1), { x: 10, y: 16, type: 130, actuator: true, wall: 350, wires: 1 },
    ]));
    d.command(2, { x: 10, y: 4, mask: 1 }); assert.equal(d.cell(10, 4).flags & 4, 0, 'seed actuator is skipped'); assert.equal(d.cell(11, 4).flags & 4, 0, 'non-solid lamp cannot become inactive');
    d.command(2, { x: 2, y: 4, mask: 1 }); assert.equal(d.cell(10, 4).flags & 4, 4); assert.equal(d.cell(11, 4).flags & 4, 0);
    d.command(2, { x: 2, y: 4, mask: 1 }); assert.equal(d.cell(10, 4).flags & 4, 0);
    d.command(2, { x: 2, y: 8, mask: 1 }); assert.equal(d.cell(10, 8).flags & 4, 0, 'chest immediately above prevents deactivation');
    d.command(2, { x: 2, y: 12, mask: 1 }); assert.equal(d.cell(10, 12).type, 130);
    d.command(2, { x: 2, y: 12, mask: 1 }); assert.equal(d.cell(10, 12).type, 131);
    d.command(2, { x: 2, y: 16, mask: 1 }); assert.equal(d.cell(10, 16).flags & 4, 4, 'DeActive short circuits when above is empty'); assert.equal(d.cell(10, 16).type, 130, 'ActiveStone still calls CanKillTile for wall 350');
  } finally { d.dispose(); }
});

test('gate output SkipWire protects its own actuator and wide support predicates see adjacent columns', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([
      ...line(2, 9, 4, 1), ...line(11, 13, 6, 8),
      { x: 10, y: 4, type: 419, fx: 36, wires: 1 }, { x: 10, y: 5, type: 419, fx: 18 },
      { x: 10, y: 6, type: 420, fx: 36, actuator: true, inactive: true, wires: 8 }, { x: 14, y: 6, type: 419, wires: 8 },
      ...line(2, 9, 12, 1), { x: 10, y: 12, type: 235, actuator: true, wires: 1 }, { x: 10, y: 11, type: 1 }, { x: 11, y: 11, type: 21 },
      ...line(2, 9, 16, 1), { x: 10, y: 16, type: 235, actuator: true, wires: 1 }, { x: 10, y: 15, type: 1 },
    ]));
    d.command(2, { x: 2, y: 4, mask: 1 }); assert.equal(d.cell(14, 6).fx, 18); assert.equal(d.cell(10, 6).flags & 4, 4, 'emitting gate is skipped by its own TripWire');
    d.command(2, { x: 13, y: 6, mask: 8 }); assert.equal(d.cell(10, 6).flags & 4, 0, 'a later external output-net pulse can reactivate the gate');
    d.command(2, { x: 2, y: 12, mask: 1 }); assert.equal(d.cell(10, 12).flags & 4, 0, 'teleporter support in the next column blocks deactivation');
    d.command(2, { x: 2, y: 16, mask: 1 }); assert.equal(d.cell(10, 16).flags & 4, 4, 'unprotected three-column footprint can deactivate');
  } finally { d.dispose(); }
});

test('multi-tile levers and momentary buttons normalize a lower-right interaction and pulse once', async () => {
  const d = await makeDriver();
  try {
    for (const type of [132, 411]) {
      const parts = []; for (let x = 4; x < 6; x++) for (let y = 4; y < 6; y++) parts.push({ x, y, type, fx: 18 * (x - 4), fy: 18 * (y - 4), wires: 1 });
      d.open(makeCircuitWorld([...parts, ...line(6, 7, 4, 1), { x: 8, y: 4, type: 419, wires: 1 }]));
      d.command(2, { x: 5, y: 5, flags: 1 }); assert.equal(d.cell(4, 4).fx, 36); assert.equal(d.cell(5, 5).fx, 54); assert.equal(d.cell(8, 4).fx, 18);
      if (type === 132) { d.command(2, { x: 5, y: 5, flags: 1 }); assert.equal(d.cell(4, 4).fx, 0); assert.equal(d.cell(8, 4).fx, 0); }
      else { d.command(3, { count: 59 }); assert.equal(d.cell(4, 4).fx, 36); d.command(3, { count: 1 }); assert.equal(d.cell(4, 4).fx, 0); assert.equal(d.cell(8, 4).fx, 18, 'release only restores the frame'); }
    }
  } finally { d.dispose(); }
});

test('cancelling a paired save during TWLD output permits a complete retry from committed state', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([{ x: 4, y: 4, type: 136, wires: 1 }, { x: 5, y: 4, type: 419, wires: 1 }]), 48 * 1024 * 1024, makeCircuitTwld());
    d.command(2, { x: 4, y: 4, flags: 1 }); assert.equal(d.cell(5, 4).fx, 18);
    assert.equal(d.cell(10, 10).flags & 8, 8);
    d.command(6, { source: 9, auxSource: 10 });
    const baselineWld = Buffer.from(d.sources.get(9)), baselineTwld = Buffer.from(d.sources.get(10));
    for (const acknowledged of [false, true]) {
      assert.equal(d.start(6, { source: 11, auxSource: 12 }), 0); let interrupted = false;
      for (let n = 0; n < 10000; n++) {
        const e = d.event(false, 1);
        if (e[1] === 2 && e[2] === 12) { if (acknowledged) d.respond(e); interrupted = true; break; }
        assert.notEqual(e[1], 4, 'the save must actually be interrupted');
        if (e[1] >= 1 && e[1] <= 3) d.respond(e);
      }
      assert.equal(interrupted, true); d.cancel(); d.sources.delete(11); d.sources.delete(12);
      assert.equal(d.cell(5, 4).fx, 18); assert.equal(d.cell(10, 10).fx, 18);
      d.command(6, { source: 11, auxSource: 12 });
      assert.deepEqual(d.sources.get(11), baselineWld); assert.deepEqual(d.sources.get(12), baselineTwld);
      d.sources.delete(11); d.sources.delete(12);
    }
    d.open(baselineWld, 48 * 1024 * 1024, baselineTwld); assert.equal(d.cell(5, 4).fx, 18); assert.equal(d.cell(10, 10).fy, 36);
  } finally { d.dispose(); }
});
