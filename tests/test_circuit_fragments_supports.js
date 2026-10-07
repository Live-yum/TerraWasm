'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { makeCircuitWorld, makeDriver } = require('./helpers/circuit-world');
const { fixture, anchorFixture, fallingFixture } = require('./helpers/circuit-supports');
const key = (x, y) => `${x},${y}`;
const byPosition = rows => new Map(rows.map(row => [key(row[0], row[1]), row]));
const activeType = row => row && row[2] & 65536 ? row[2] & 65535 : null;

test('natural trap fragments strip cave decor, retain exact anchors, and remain bounded in compiled WASM', async () => {
  const d = await makeDriver(), f = fixture();
  try {
    d.open(f.world);
    const identity = JSON.parse(d.M.UTF8ToString(d.M._terra_build_info_json()));
    assert.equal(identity.circuitWorldFragmentSupports, 1, 'the compiled artifact claims the exercised support ABI');
    d.start(7, { count: 2, rawRecords: f.geometry }); d.event(false, 1); d.cancel();
    const first = d.command(7, { count: 2 }), descriptors = [...first.rows];
    assert.equal(first.event[10], 11);
    for (let x = 2; x < first.event[10]; x += 2) descriptors.push(...d.command(7, { x, count: 2 }).rows);
    assert.equal(descriptors.length, 11); assert.ok(descriptors.every(row => !(row[7] & 9)), 'all source objects and their anchors are complete');
    const select = (x, y) => {
      const descriptor = descriptors.find(row => row[1] === x && row[2] === y); assert.ok(descriptor, `fragment at ${x},${y}`);
      if (descriptor[5] > 1) assert.equal(d.start(8, { mask: descriptor[0], count: descriptor[5] - 1 }), -4, 'check the entire object plus support budget before output');
      const result = d.command(8, { mask: descriptor[0], count: descriptor[5] });
      assert.equal(result.rows.length, descriptor[5]); assert.equal(result.event[10], descriptor[5]);
      return { descriptor, rows: result.rows, cells: byPosition(result.rows) };
    };
    const trap = select(2, 6); assert.equal(trap.rows.length, 12); assert.equal(trap.descriptor[6], 11);
    assert.equal(activeType(trap.cells.get('4,6')), 135); assert.equal(activeType(trap.cells.get('12,6')), 137);
    assert.equal(activeType(trap.cells.get('4,7')), 54); assert.equal(trap.cells.get('4,7')[4] >>> 16, 7, 'keep source glass and paint');
    for (let x = 5; x <= 10; x++) {
      const row = trap.cells.get(key(x, 6)); assert.ok(row); assert.equal(activeType(row), null, 'stone, pots and stalactites along a wire are not devices');
      assert.equal(row[4], 0); assert.equal(row[5] & 0xffffff, 0, 'unrelated wall, paint and liquid layers stay in the source');
      assert.equal(row[5] >>> 24, 1);
    }
    const crossing = select(8, 3); assert.equal(crossing.rows.length, 7); assert.ok(crossing.rows.every(row => row[5] >>> 24 === 2));
    const boulder = select(2, 13); assert.equal(boulder.rows.length, 14);
    for (const at of ['10,13', '11,13', '10,14', '11,14']) assert.equal(activeType(boulder.cells.get(at)), 138, 'an unwired boulder belongs to its wired release support');
    assert.equal(activeType(boulder.cells.get('10,15')), 130); assert.equal(activeType(boulder.cells.get('11,15')), 1);
    const candle = select(20, 10); assert.equal(candle.rows.length, 10);
    for (let x = 20; x <= 22; x++) { assert.equal(activeType(candle.cells.get(key(x, 11))), 14); assert.equal(activeType(candle.cells.get(key(x, 12))), 14); assert.equal(activeType(candle.cells.get(key(x, 13))), 54); }
    const door = select(2, 19); assert.equal(door.rows.length, 5); assert.equal(activeType(door.cells.get('2,19')), 54); assert.equal(activeType(door.cells.get('2,23')), 54); assert.ok(!door.cells.has('3,21'));
    const side = select(7, 21); assert.equal(side.rows.length, 2); assert.equal(activeType(side.cells.get('7,21')), 1); assert.ok(!side.cells.has('9,21'), 'choose one valid original switch anchor');
    const lever = select(12, 20); assert.equal(lever.rows.length, 4); assert.ok(lever.rows.every(row => row[4] === (2 | 9 << 24) >>> 0), 'a wall-mounted lever needs every original wall cell');
    const chandelier = select(20, 19); assert.equal(chandelier.rows.length, 10); assert.ok(chandelier.cells.has('21,19')); assert.ok(!chandelier.cells.has('20,19') && !chandelier.cells.has('22,19'));
    const cannon = select(28, 20); assert.equal(cannon.rows.length, 14); assert.ok(cannon.cells.has('29,23') && cannon.cells.has('30,23')); assert.ok(!cannon.cells.has('28,23') && !cannon.cells.has('31,23'));
    const chest = select(28, 4); assert.equal(chest.rows.length, 6); assert.equal(chest.descriptor[7], 2, 'preview retains complete tiles while requiring chest inventory for a writable transfer');
    const torch = select(36, 12); assert.equal(torch.rows.length, 1); assert.equal(torch.rows[0][4], (2 | 5 << 16 | 9 << 24) >>> 0);
    assert.ok(d.command(6, { source: 9 }).event[10] > 0, 'extraction and budget rejection leave SAVE usable');
  } finally { d.dispose(); }
});

test('source-specific anchor alternatives preserve trees, platforms, boulder holds and stalactite materials', async () => {
  const d = await makeDriver(), f = anchorFixture();
  try {
    d.open(f.world); const list = d.command(7, { count: 32, rawRecords: f.geometry });
    assert.equal(list.rows.length, 17);
    const expected = [[4,4,4,0],[12,5,1,8],[17,5,2,0],[24,4,3,0],[30,4,3,0],[36,4,5,0],[42,4,4,10],[50,3,3,0],[55,4,2,8],[60,3,2,0],[4,15,4,0],[12,16,2,0],[18,16,1,8],[24,16,1,8],[29,15,4,0],[0,0,9,8],[24,0,3,8]];
    const output = new Map();
    for (const [x, y, count, flags] of expected) {
      const row = list.rows.find(r => r[1] === x && r[2] === y); assert.ok(row, `fragment at ${x},${y}`);
      assert.equal(row[5], count, `exact anchor cells at ${x},${y}`); assert.equal(row[7], flags, `source anchor validity at ${x},${y}`);
      const extracted = d.command(8, { mask: row[0], count }).rows; assert.equal(extracted.length, count); output.set(key(x, y), byPosition(extracted));
    }
    for (const [x, y, fragment] of [[4,4,'4,4'],[4,15,'4,15'],[29,15,'29,15']]) for (let dy = 0; dy < 3; dy++) assert.equal(activeType(output.get(fragment).get(key(x, y + dy))), 5, 'side mounts need all three original tree cells');
    assert.equal(activeType(output.get('17,5').get('17,5')), 124);
    assert.equal(activeType(output.get('24,4').get('24,4')), 427); assert.equal(activeType(output.get('30,4').get('30,4')), 435);
    assert.equal(activeType(output.get('36,4').get('36,6')), 130); assert.ok(!output.get('36,4').has('37,6'));
    assert.ok(!output.get('42,4').has('42,6'), 'a chest cannot keep an invalid tabletop as a floor');
    assert.equal(activeType(output.get('50,3').get('50,3')), 1); assert.ok(!output.get('55,4').has('55,3'));
    assert.equal(activeType(output.get('60,3').get('60,3')), 225);
    assert.equal(activeType(output.get('0,0').get('0,0')), 34, 'the complete boundary object survives');
    assert.ok(!output.get('24,0').has('24,3'), 'one valid foot cannot replace the missing out-of-world ceiling');
  } finally { d.dispose(); }
});

test('missing anchors are distinct from broken geometry; invalid anchor ABI and legacy geometry stay explicit', async () => {
  const d = await makeDriver();
  const cells = [{ x: 5, y: 6, type: 10, wires: 1 }, { x: 5, y: 7, type: 10, fy: 18 }, { x: 5, y: 8, type: 10, fy: 36 }];
  const geometry = [0, 1, 2].map(dy => [10, dy * 18 << 16, (dy << 8 | 1 << 16 | 3 << 24) >>> 0, 3]);
  try {
    d.open(makeCircuitWorld(cells));
    const bad = geometry.map(row => [...row]); bad[0][3] = 18;
    assert.equal(d.start(7, { count: 8, rawRecords: bad }), -1);
    const list = d.command(7, { count: 8, rawRecords: geometry }); assert.equal(list.rows.length, 1); assert.equal(list.rows[0][7], 8);
    assert.equal(d.command(8, { mask: list.rows[0][0], count: 3 }).rows.length, 3, 'native reports a missing anchor without inventing terrain');
    d.open(makeCircuitWorld(cells));
    const legacy = geometry.map(row => [...row.slice(0, 3), 0]);
    assert.equal(d.command(7, { count: 8, rawRecords: legacy }).rows[0][7], 0, 'zero remains the legacy no-additional-anchor layout');
    for (const base of [{ type: 54 }, { type: 19, fx: 8 * 18 }]) {
      d.open(makeCircuitWorld([{ x: 5, y: 6, type: 33, wires: 1 }, { x: 5, y: 7, ...base }]));
      const candle = d.command(7, { count: 8, rawRecords: [[33, 0, (1 << 16 | 1 << 24) >>> 0, 14]] });
      assert.equal(candle.rows[0][7], 8, 'glass and a platform without a proper top cannot satisfy a candle table anchor');
    }
  } finally { d.dispose(); }
});

test('falling materials retain only their existing stopping chain without filling an intentional falling path', async () => {
  const d = await makeDriver(), f = fallingFixture();
  try {
    d.open(f.world); const list = d.command(7, { count: 32, rawRecords: f.geometry }); assert.equal(list.rows.length, 9);
    const expected = [[5,5,44,0],[15,5,3,0],[20,5,1,8],[25,5,1,0],[30,5,1,8],[35,5,6,2],[42,5,3,0],[48,5,2,8],[53,5,2,0]], output = new Map();
    for (const [x, y, count, flags] of expected) {
      const descriptor = list.rows.find(row => row[1] === x && row[2] === y); assert.ok(descriptor);
      assert.equal(descriptor[5], count, `exact stopping chain at ${x},${y}`); assert.equal(descriptor[7], flags, `falling source state at ${x},${y}`);
      const rows = d.command(8, { mask: descriptor[0], count }).rows; assert.equal(rows.length, count); output.set(key(x, y), byPosition(rows));
    }
    for (let y = 6; y <= 47; y++) assert.equal(activeType(output.get('5,5').get(key(5, y))), 123, 'a long source chain is copied without repeated object-depth expansion');
    assert.equal(activeType(output.get('5,5').get('5,48')), 1); assert.ok(!output.get('5,5').has('6,48'));
    assert.equal(activeType(output.get('15,5').get('15,7')), 1); assert.ok(!output.get('15,5').has('15,8'));
    for (const x of [20, 25, 30]) assert.ok(!output.get(key(x, 5)).has(key(x, 6)), 'never add a blocking base to an original falling path');
    assert.ok(output.get('25,5').get('25,5')[2] & 4 << 16, 'preserve an inactive payload');
    assert.equal(activeType(output.get('35,5').get('36,6')), 21); assert.ok(!output.get('35,5').has('35,8'), 'the original chest holds the sand from above');
    assert.equal(activeType(output.get('42,5').get('42,6')), 136); assert.equal(activeType(output.get('42,5').get('42,7')), 1);
    assert.equal(activeType(output.get('48,5').get('48,6')), 53); assert.ok(!output.get('53,5').has('54,6'), 'a stable side anchor wins over an unsupported sand alternative');
  } finally { d.dispose(); }
});

function stamp(d, rows, rectangle) {
  const M = d.M, pointers = [], alloc = data => { const p = M._tx_malloc(typeof data === 'number' ? data : data.length); assert.ok(p); pointers.push(p); if (typeof data !== 'number') M.HEAPU8.set(data, p); return p; };
  const string = text => alloc(Buffer.from(text + '\0')), hp = alloc(4), ep = alloc(48), lease = alloc(32);
  const data = Buffer.from(new Uint32Array(rows.flat()).buffer), chunks = []; let task = 0;
  try {
    const request = { ...rectangle, recordCount: rows.length, recordSourceId: 9, mode: 'overlay' };
    assert.equal(M._terra_world_stream_operation_begin(d.worldHandle, string('stamp_tiles'), string(JSON.stringify(request)), hp), 0); task = M.HEAPU32[hp >>> 2];
    for (let n = 0; n < 100000; n++) {
      assert.ok(M._terra_world_stream_step(task, 31, ep) >= 0);
      const e = Array.from(M.HEAPU32.subarray(ep >>> 2, (ep >>> 2) + 12));
      if (e[1] === 1) {
        const source = e[2] === 9 ? data : d.sources.get(e[2]); assert.ok(source); assert.ok(e[3] + e[4] <= source.length);
        assert.equal(M._terra_world_stream_acquire_input(task, lease), 0); const l = Array.from(M.HEAPU32.subarray(lease >>> 2, (lease >>> 2) + 8));
        M.HEAPU8.set(source.subarray(e[3], e[3] + e[4]), l[5]); assert.equal(M._terra_world_stream_commit_input(task, l[1], e[2], e[3], e[4]), 0);
      } else if (e[1] === 3) { chunks.push([e[3], Buffer.from(M.HEAPU8.subarray(e[5], e[5] + e[4]))]); assert.equal(M._terra_world_stream_ack_output(task), 0); }
      else if (e[1] === 4) { const output = Buffer.alloc(e[10]); for (const [offset, bytes] of chunks) bytes.copy(output, offset); return output; }
    }
    assert.fail('bounded sparse support stamp did not finish');
  } finally { if (task) assert.equal(M._terra_world_stream_close(task), 0); for (const p of pointers) M._tx_free(p); }
}

test('door anchors survive fragment extraction, the real stream writer and WLD reopen', async () => {
  const d = await makeDriver(), f = fixture();
  try {
    d.open(f.world); const descriptor = d.command(7, { count: 32, rawRecords: f.geometry }).rows.find(row => row[1] === 2 && row[2] === 19); assert.ok(descriptor);
    const rows = d.command(8, { mask: descriptor[0], count: 5 }).rows.map(row => [row[0] - descriptor[1], row[1] - descriptor[2], ...row.slice(2)]);
    d.open(makeCircuitWorld([], 40, 32, 326)); const baseline = d.M._tx_native_heap_used();
    const output = stamp(d, rows, { x: 10, y: 5, width: 1, height: 5 }); assert.equal(d.M._tx_native_heap_used(), baseline);
    d.open(output);
    for (const y of [5, 9]) { assert.equal(d.cell(10, y).type, 54); assert.equal(d.cell(10, y).flags & 1, 1); }
    for (let y = 6; y <= 8; y++) { const tile = d.cell(10, y); assert.equal(tile.type, 10); assert.equal(tile.fy, (y - 6) * 18); }
    assert.equal(d.cell(10, 7).wires, 2); assert.equal(d.cell(11, 7).flags & 1, 0);
  } finally { d.dispose(); }
});
