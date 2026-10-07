'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { makeCircuitWorld, makeDriver } = require('./helpers/circuit-world');
const fixtureBytes = fs.readFileSync(path.join(__dirname, 'fixtures/circuit-hello.json'));
const fixture = JSON.parse(fixtureBytes);
const sha256 = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const fixtureHash = 'b8ad007507b2f70baeed3fb47dc2006559e0f8a5f40ce0943c937ca75d31c67c';
// Independent expected pixels, not output from the frontend simulator.
const frames = [
  ['10001', '10001', '11111', '10001', '10001'],
  ['11111', '10000', '11110', '10000', '11111'],
  ['10000', '10000', '10000', '10000', '11111'],
  ['10000', '10000', '10000', '10000', '11111'],
  ['01110', '10001', '10001', '10001', '01110'],
];
const margin = 4;
function world(cells = fixture.cells) {
  return makeCircuitWorld(cells.map(cell => ({ ...cell, x: cell.x + margin, y: cell.y + margin })), fixture.width + margin * 2, fixture.height + margin * 2, fixture.worldVersion);
}
const cell = (d, x, y) => d.cell(x + margin, y + margin);
const toggle = d => d.command(2, { x: fixture.layout.switch.x + margin, y: fixture.layout.switch.y + margin, flags: 1 });
const step = (d, count) => d.command(3, { count });
function display(d, index, label) {
  const actual = [];
  for (let y = 0; y < 5; y++) {
    let row = '';
    for (let x = 0; x < 5; x++) {
      const pixel = cell(d, 3 + x * 2, 3 + y * 2);
      assert.ok(pixel.flags & 1 && [260, 267].includes(pixel.type), `${label}: white gemspark ${x},${y}`);
      row += pixel.type === 267 ? '1' : '0';
    }
    actual.push(row);
  }
  assert.deepEqual(actual, frames[index], label);
  assert.deepEqual(fixture.layout.states.map(point => cell(d, point.x, point.y).fx),
    Array.from({ length: 5 }, (_, i) => i === index ? 18 : 0), `${label}: independent state for both L slots`);
}

test('real exported HELLO circuit advances three cycles on native half-second timers, pauses and survives WLD save', async t => {
  assert.equal(sha256(fixtureBytes), fixtureHash, 'update the pinned fixture only through the viewer export generator');
  assert.equal(fixture.worldVersion, 326);
  assert.equal(fixture.layout.periodTicks, 30);
  const d = await makeDriver(), started = Date.now();
  try {
    d.open(world());
    const p = d.M._terra_build_info_json(), end = d.M.HEAPU8.indexOf(0, p);
    assert.ok(p > 0 && end > p && end - p < 65536);
    const identity = JSON.parse(Buffer.from(d.M.HEAPU8.subarray(p, end)).toString('utf8'));
    if (process.env.TCW_EXPECTED_COMMIT) {
      assert.equal(identity.sourceCommit, process.env.TCW_EXPECTED_COMMIT);
      assert.equal(identity.dirty, false);
    }
    assert.equal(cell(d, 18, 9).type, 54, 'real glass switch anchor');
    assert.equal(cell(d, 22, 8).fx, 54, 'native half-second timer style');
    display(d, 0, 'initial H'); toggle(d); display(d, 0, 'start does not shift');
    for (let slot = 1; slot <= 15; slot++) {
      step(d, 29); display(d, (slot - 1) % 5, `slot ${slot} before deadline`);
      step(d, 1); display(d, slot % 5, `slot ${slot} deadline`);
    }
    step(d, 7); toggle(d); step(d, 90); display(d, 0, 'stopped display holds');
    assert.equal(cell(d, 22, 8).fy, 0);
    toggle(d); step(d, 29); display(d, 0, 'restart waits the full interval');
    step(d, 1); display(d, 1, 'restart E');
    step(d, 7); toggle(d); toggle(d); step(d, 22); display(d, 1, 'same-tick toggle preserves native timer phase');
    step(d, 1); display(d, 2, 'remaining interval expires');
    toggle(d); step(d, 1);
    d.command(6, { source: 9 });
    const saved = d.sources.get(9); assert.ok(saved.length > 0);
    d.open(saved); display(d, 2, 'saved world retains pixels and ring state');
    assert.equal(cell(d, 22, 8).fy, 0, 'world reload follows native timer-off rule');
    toggle(d); step(d, 29); display(d, 2, 'reloaded world before deadline');
    step(d, 1); display(d, 3, 'reloaded world advances to the second L state');
    const report = { status: 'passed', fixtureSha256: fixtureHash, sourceSha256: fixture.sourceSha256,
      wasmBuild: identity, wasmSha256: sha256(fs.readFileSync(path.join(process.env.TCW_BUILD_DIR || path.join(__dirname, '../build'), 'terrax_world_wasm_web.wasm'))),
      display: { columns: 5, rows: 5, physicalCells: 25, sequence: 'HELLO', periodTicks: 30, cycles: 3 },
      checks: ['29+1 tick boundary', 'stop holds 90 ticks', 'restart after pause', 'same-tick toggle native phase', 'WLD save/reopen'],
      savedWorldBytes: saved.length, elapsedMs: Date.now() - started };
    if (process.env.TCW_HELLO_REPORT) fs.writeFileSync(process.env.TCW_HELLO_REPORT, JSON.stringify(report, null, 2) + '\n');
    t.diagnostic(JSON.stringify(report));
  } finally { d.dispose(); }
});

test('cutting the exported clock wire prevents HELLO state changes in actual WASM', async () => {
  const d = await makeDriver();
  try {
    const broken = fixture.cells.map(cell => cell.x === 22 && cell.y === 10 ? { ...cell, wires: cell.wires & ~2 } : cell);
    assert.notDeepEqual(broken, fixture.cells, 'negative control removes a real clock wire');
    d.open(world(broken)); toggle(d); step(d, 150);
    assert.equal(cell(d, 22, 8).fy, 18, 'timer is actually enabled');
    display(d, 0, 'disconnected physical ring stays at H');
  } finally { d.dispose(); }
});
