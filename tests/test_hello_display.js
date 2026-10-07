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
const fixtureHash = '703672f3d8b3cefdae779b98c0ce181276a02d378a60040984bf6ae07fdcf4a3';
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
const screen = fixture.layout.screen;
const physicalFrame = index => frames[index].flatMap(row =>
  Array.from({ length: screen.pixelSize }, () => [...row].map(bit => bit.repeat(screen.pixelSize)).join('')));
function readScreen(d, label) {
  const actual = [];
  for (let y = 0; y < screen.height; y++) {
    let row = '';
    for (let x = 0; x < screen.width; x++) {
      const pixel = cell(d, screen.x + x, screen.y + y);
      assert.ok(pixel.flags & 1 && [260, 267].includes(pixel.type), `${label}: every physical cell is white gemspark, including ${x},${y}`);
      row += pixel.type === 267 ? '1' : '0';
    }
    actual.push(row);
  }
  return actual;
}
function state(d, index, label) {
  assert.deepEqual(fixture.layout.states.map(point => cell(d, point.x, point.y).fx),
    Array.from({ length: 5 }, (_, i) => i === index ? 18 : 0), `${label}: independent state for both L slots`);
}
function display(d, index, label) {
  assert.deepEqual(readScreen(d, label), physicalFrame(index), label);
  state(d, index, label);
}

test('real exported HELLO circuit advances three cycles on native half-second timers, pauses and survives WLD save', async t => {
  assert.equal(sha256(fixtureBytes), fixtureHash, 'update the pinned fixture only through the viewer export generator');
  assert.equal(fixture.worldVersion, 326);
  assert.equal(fixture.layout.periodTicks, 30);
  assert.deepEqual([screen.columns, screen.rows, screen.pixelSize], [5, 5, 2]);
  assert.equal(screen.pitch, screen.pixelSize, 'logical pixels touch without air lanes');
  assert.equal(screen.width, screen.columns * screen.pixelSize);
  assert.equal(screen.height, screen.rows * screen.pixelSize);
  assert.equal(screen.width * screen.height, 100, 'the entire square has 100 physical display cells');
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
      display: { columns: screen.columns, rows: screen.rows, width: screen.width, height: screen.height,
        pixelSize: screen.pixelSize, physicalCells: screen.width * screen.height, gaps: 0, sequence: 'HELLO', periodTicks: 30, cycles: 3 },
      checks: ['all 100 physical cells in every frame', '29+1 tick boundary', 'stop holds 90 ticks', 'restart after pause', 'same-tick toggle native phase', 'WLD save/reopen'],
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

test('cutting the display feed freezes all 100 screen cells while the native state ring continues', async () => {
  const d = await makeDriver();
  try {
    const broken = fixture.cells.map(cell => cell.x === 0 && cell.y === 1 ? { ...cell, wires: 0 } : cell);
    assert.notDeepEqual(broken, fixture.cells, 'negative control removes the real display feed');
    d.open(world(broken)); toggle(d);
    for (let slot = 1; slot <= 5; slot++) {
      step(d, 30);
      state(d, slot % 5, `disconnected display, slot ${slot}`);
      assert.deepEqual(readScreen(d, 'display feed cut'), physicalFrame(0), 'the display remains H even while the ring advances');
    }
    assert.equal(cell(d, fixture.layout.timer.x, fixture.layout.timer.y).fy, 18);
  } finally { d.dispose(); }
});

test('cutting one display cell wire changes exactly that physical pixel, not its 2x2 logical neighbours', async () => {
  const d = await makeDriver();
  try {
    const x = 5, y = 3;
    const broken = fixture.cells.map(cell => cell.x === x && cell.y === y ? { ...cell, wires: cell.wires & ~2 } : cell);
    assert.notDeepEqual(broken, fixture.cells, 'negative control removes the blue wire from one display cell');
    d.open(world(broken)); toggle(d); step(d, 30); state(d, 1, 'E state still advances');
    const expected = physicalFrame(1), actual = readScreen(d, 'one pixel wire cut');
    assert.equal(actual[0].replaceAll('1', '#').replaceAll('0', '.'), '##.#######');
    const differences = [];
    for (let row = 0; row < screen.height; row++) for (let col = 0; col < screen.width; col++) {
      if (actual[row][col] !== expected[row][col]) differences.push([screen.x + col, screen.y + row]);
    }
    assert.deepEqual(differences, [[x, y]], 'the other 99 physical cells still display the correct E');
  } finally { d.dispose(); }
});
