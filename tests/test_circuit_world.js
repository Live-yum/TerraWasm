'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { makeCircuitWorld, makeCircuitTwld, makeDriver } = require('./helpers/circuit-world');
const line = (x1, x2, y, wires) => Array.from({ length: x2 - x1 + 1 }, (_, n) => ({ x: x1 + n, y, wires }));

test('COB1 preserves all 18 section-backed objects and exact long strings through the real WASM writer', async () => {
  const { fixture, sectionRecords, replaceSections } = require('./helpers/circuit-objects');
  const f = fixture(), d = await makeDriver(), M = d.M, pointers = [];
  const alloc = bytes => { const p = M._tx_malloc(typeof bytes === 'number' ? bytes : bytes.length); assert.ok(p); pointers.push(p); if (typeof bytes !== 'number') M.HEAPU8.set(bytes, p); return p; };
  const str = s => alloc(Buffer.from(s + '\0')), hp = alloc(4), ep = alloc(48), lease = alloc(32); let task = 0;
  function stamp(records, companion, expectFailure = false) {
    const cells = Buffer.from(new Uint32Array(records.flat()).buffer), pieces = [];
    const request = { x: 3, y: 20, width: 90, height: 4, recordCount: records.length, recordSourceId: 9, mode: 'overlay', objectSourceId: 10, objectBytes: companion.length, objectCount: companion.readUInt32LE(12) };
    const status = M._terra_world_stream_operation_begin(d.worldHandle, str('stamp_tiles'), str(JSON.stringify(request)), hp);
    if (status < 0) { assert.ok(expectFailure); return null; } task = M.HEAPU32[hp >>> 2];
    try {
      for (let n = 0; n < 100000; n++) {
        const status = M._terra_world_stream_step(task, 7, ep);
        if (status < 0) { assert.ok(expectFailure); assert.equal(pieces.length, 0, 'invalid metadata fails before output'); return null; }
        const e = Array.from(M.HEAPU32.subarray(ep >>> 2, (ep >>> 2) + 12));
        if (e[1] === 1) {
          const source = e[2] === 9 ? cells : e[2] === 10 ? companion : d.sources.get(e[2]); assert.ok(source); assert.ok(e[3] + e[4] <= source.length);
          assert.equal(M._terra_world_stream_acquire_input(task, lease), 0);
          const l = Array.from(M.HEAPU32.subarray(lease >>> 2, (lease >>> 2) + 8));
          M.HEAPU8.set(source.subarray(e[3], e[3] + e[4]), l[5]); assert.equal(M._terra_world_stream_commit_input(task, l[1], e[2], e[3], e[4]), 0);
        } else if (e[1] === 3) { pieces.push([e[3], Buffer.from(M.HEAPU8.subarray(e[5], e[5] + e[4]))]); assert.equal(M._terra_world_stream_ack_output(task), 0); }
        else if (e[1] === 4) { assert.ok(!expectFailure); const out = Buffer.alloc(e[10]); for (const [at, bytes] of pieces) bytes.copy(out, at); return out; }
      } assert.fail('object stamp did not finish');
    } finally { assert.equal(M._terra_world_stream_close(task), 0); task = 0; }
  }
  const part = (bytes, section) => bytes.subarray(bytes.readUInt32LE(26 + section * 4), section === 10 ? bytes.length : bytes.readUInt32LE(30 + section * 4));
  try {
    d.open(f.world); const list = d.command(7, { count: 8, rawRecords: f.geometry }); assert.equal(list.rows.length, 1); assert.equal(list.rows[0][7], 2);
    const before = M._tx_native_heap_used();
    const extracted = d.command(8, { mask: list.rows[0][0], count: 512, flags: 1, auxSource: 11, width: 4 * 1024 * 1024, height: 32768 });
    const companion = d.sources.get(11); assert.deepEqual(companion, f.companion); assert.ok(companion.length > 65536);
    const records = extracted.rows.map(row => [row[0] - 3, row[1] - 4, ...row.slice(2)]);
    const baseline = M._tx_native_heap_used(), output = stamp(records, companion); assert.equal(M._tx_native_heap_used(), baseline);
    for (const section of [2,3,5]) {
      const old = part(f.world, section), prefix = section === 5 ? 4 : 2, appended = sectionRecords(f.objects, section, 0, 16, 111, true);
      const expected = Buffer.concat([old.subarray(0, prefix), old.subarray(prefix), appended.subarray(prefix)]);
      if (prefix === 4) expected.writeUInt32LE(old.readUInt32LE(0) * 2, 0); else expected.writeUInt16LE(old.readUInt16LE(0) * 2, 0);
      assert.deepEqual(part(output, section), expected, `section ${section} keeps every original byte and complete appended payload`);
    }
    for (const section of [0,4,6,7,8,9,10]) assert.deepEqual(part(output, section), part(f.world, section));
    for (const [offset, value] of [[0,0],[8,325],[28,1],[32+16,1],[32+8,90]]) { const bad = Buffer.from(companion); bad.writeUInt32LE(value, offset); assert.equal(stamp(records, bad, true), null); assert.equal(M._tx_native_heap_used(), baseline); }
    const missing = records.map(row => row.slice()), cell = missing.find(row => (row[2] & 65535) === 378 && row[1] === 2); cell[2] = 0;
    assert.equal(stamp(missing, companion, true), null);
    d.open(output); assert.equal(stamp(records, companion, true), null, 'existing destination inventory cannot be overwritten');
    d.open(makeCircuitWorld([], 100, 32)); assert.equal(stamp(records, companion, true), null, 'nonempty object payloads cannot be written into old layouts');
    // A missing source record fails extraction, never silently creates empty storage.
    d.open(replaceSections(f.world, { 2: Buffer.alloc(2) })); const missingList = d.command(7, { count: 8, rawRecords: f.geometry });
    assert.throws(() => d.command(8, { mask: missingList.rows[0][0], count: 512, flags: 1, auxSource: 11, width: 4 * 1024 * 1024, height: 32768 }), /native status/);
    assert.ok(before > 0);
  } finally { if (task) M._terra_world_stream_close(task); d.dispose(); for (const p of pointers) M._tx_free(p); }
});

test('old tile-only worlds can request an empty bounded COB1 companion', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld(line(3, 8, 4, 1))); const list = d.command(7, { count: 8 });
    d.command(8, { mask: list.rows[0][0], count: 8, flags: 1, auxSource: 11, width: 32, height: 1 });
    const bytes = d.sources.get(11); assert.equal(bytes.length, 32); assert.equal(bytes.readUInt32LE(8), 196); assert.equal(bytes.readUInt32LE(12), 0);
  } finally { d.dispose(); }
});

test('fragment enumeration keeps crossing colors separate and completes exact multi-tile layouts', async () => {
  const d = await makeDriver();
  try {
    const cells = new Map(), put = (x, y, t) => cells.set(`${x},${y}`, { ...(cells.get(`${x},${y}`) || {}), x, y, ...t });
    for (let x = 3; x <= 9; x++) put(x, 4, { wires: 1 });
    for (let y = 2; y <= 8; y++) put(6, y, { wires: y === 4 ? 3 : 2 });
    const geometry = [];
    for (let x = 0; x < 2; x++) for (let y = 0; y < 2; y++) {
      put(12 + x, 10 + y, { type: 132, fx: x * 18, fy: y * 18, paint: 7, wall: 2, wallPaint: 9, wires: x === y ? 1 << x : 0 });
      geometry.push([132, x * 18 | (y * 18 << 16), x | (y << 8) | (2 << 16) | (2 << 24), 0]);
    }
    put(11, 10, { wires: 1 }); put(14, 11, { wires: 2 });
    put(20, 5, { type: 419, wires: 1 }); put(20, 6, { type: 419 }); put(20, 7, { type: 420, wires: 2 });
    d.open(makeCircuitWorld([...cells.values()]));
    assert.equal(d.start(7, { count: 1, rawRecords: geometry }), 0);
    d.event(false, 1); d.cancel(); // retry retains the validated geometry table
    const first = d.command(7, { count: 1 }); assert.equal(first.event[10], 4);
    const descriptors = [...first.rows, ...d.command(7, { x: 1, count: 8 }).rows];
    const pure = descriptors.find(r => r[1] === 3), object = descriptors.find(r => r[1] === 11), stack = descriptors.find(r => r[1] === 20);
    assert.equal(pure[5], 7); assert.equal(object[5], 6); assert.equal(stack[5], 3); assert.ok(descriptors.every(r => r[7] === 0));
    const extracted = d.command(8, { mask: pure[0], count: 32 }); assert.equal(extracted.event[10], 7);
    assert.ok(extracted.rows.every(r => r[5] >>> 24 === 1), 'the other color at the crossing is absent');
    assert.equal(d.start(8, { mask: object[0], count: 5 }), -4, 'reject the whole selection before any truncated output');
    assert.ok(d.command(6, { source: 9 }).event[10] > 0, 'a rejected extraction must not poison a subsequent save');
    const complete = d.command(8, { mask: object[0], count: 6 }).rows.filter(r => (r[2] & 65535) === 132);
    assert.equal(complete.length, 4); assert.ok(complete.every(r => r[4] === (2 | 7 << 16 | 9 << 24)));
  } finally { d.dispose(); }
});

test('empty worlds and pure-wire graphs enumerate without allocating object layouts', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([])); const empty = d.command(7, { count: 16 }); assert.equal(empty.event[10], 0); assert.deepEqual(empty.rows, []);
    d.open(makeCircuitWorld(line(4, 8, 4, 1))); const list = d.command(7, { count: 16 }); assert.equal(list.event[10], 1); assert.equal(list.rows[0][5], 5);
  } finally { d.dispose(); }
});

test('ambiguous or broken frame layouts and section-backed objects are explicitly marked', async () => {
  const d = await makeDriver();
  try {
    d.open(makeCircuitWorld([{ x: 5, y: 5, type: 132, wires: 1 }, { x: 10, y: 5, type: 21, wires: 2 }]));
    const geometry = [[132, 0, 2 << 16 | 2 << 24, 0]];
    const rows = d.command(7, { count: 8, rawRecords: geometry }).rows;
    assert.equal(rows.length, 2); assert.equal(rows.find(r => r[1] === 5)[7] & 1, 1, 'missing object cells are not replaced with bbox terrain');
    assert.equal(rows.find(r => r[1] === 10)[7] & 2, 2, 'a chest cannot silently lose its inventory');
  } finally { d.dispose(); }
});

test('every pinned container, sign and tile entity is marked as requiring section data', async () => {
  const d = await makeDriver();
  try {
    const types = [21, 55, 85, 88, 378, 395, 423, 425, 467, 470, 471, 475, 520, 573, 597, 698, 723, 724];
    d.open(makeCircuitWorld(types.map((type, i) => ({ x: 3 + i * 2, y: 5, type, wires: 1 }))));
    const list = d.command(7, { count: types.length });
    assert.equal(list.event[10], types.length); assert.equal(list.rows.length, types.length);
    for (let i = 0; i < types.length; i++) {
      const descriptor = list.rows.find(row => row[1] === 3 + i * 2);
      assert.ok(descriptor); assert.equal(descriptor[7] & 2, 2, `tile ${types[i]} cannot be copied without its section record`);
    }
  } finally { d.dispose(); }
});

test('stamp_tiles replays bounded source windows, preserves terrain and metadata, and cancels without adoption', async () => {
  const d = await makeDriver(), M = d.M, pointers = [];
  const alloc = data => { const p = M._tx_malloc(typeof data === 'number' ? data : data.length); assert.ok(p); pointers.push(p); if (typeof data !== 'number') M.HEAPU8.set(data, p); return p; };
  const string = value => alloc(Buffer.from(value + '\0'));
  const hp = alloc(4), ep = alloc(48), lease = alloc(32); let task = 0;
  const records = new Uint32Array(5000 * 8);
  for (let x = 0; x < 100; x++) for (let y = 0; y < 50; y++) { const i = (x * 50 + y) * 8; records[i] = x; records[i + 1] = y; records[i + 5] = 1 << 24; }
  records[50 * 8 + 2] = 314 | 1 << 16; records[50 * 8 + 3] = 0xffff0000;
  const bytes = Buffer.from(records.buffer), request = { x: 10, y: 10, width: 100, height: 50, recordCount: 5000, recordSourceId: 9, mode: 'overlay' };
  function begin() { assert.equal(M._terra_world_stream_operation_begin(d.worldHandle, string('stamp_tiles'), string(JSON.stringify(request)), hp), 0); task = M.HEAPU32[hp >>> 2]; }
  function pump(expectFailure = false) {
    const pieces = []; let stampReads = 0;
    for (let n = 0; n < 100000; n++) {
      const status = M._terra_world_stream_step(task, 23, ep); if (status < 0) { assert.ok(expectFailure); return null; }
      const e = Array.from(M.HEAPU32.subarray(ep >>> 2, (ep >>> 2) + 12));
      if (e[1] === 1) {
        const source = e[2] === 9 ? bytes : d.sources.get(e[2]); assert.ok(source);
        if (e[2] === 9) { stampReads++; assert.ok(e[4] <= 65536); }
        assert.equal(M._terra_world_stream_acquire_input(task, lease), 0);
        const l = Array.from(M.HEAPU32.subarray(lease >>> 2, (lease >>> 2) + 8));
        M.HEAPU8.set(source.subarray(e[3], e[3] + e[4]), l[5]);
        assert.equal(M._terra_world_stream_commit_input(task, l[1], e[2], e[3], e[4]), 0);
      } else if (e[1] === 3) { pieces.push([e[3], Buffer.from(M.HEAPU8.subarray(e[5], e[5] + e[4]))]); assert.equal(M._terra_world_stream_ack_output(task), 0); }
      else if (e[1] === 4) { assert.ok(!expectFailure); const output = Buffer.alloc(e[10]); for (const [offset, value] of pieces) value.copy(output, offset); assert.ok(stampReads >= 6); return output; }
    }
    assert.fail('stamp exceeded bounded execution');
  }
  try {
    d.open(makeCircuitWorld([{ x: 10, y: 10, type: 1, wires: 2, paint: 4, wall: 3, wallPaint: 6 }], 120, 80));
    const original = Buffer.from(d.sources.get(1)), baseline = M._tx_native_heap_used();
    begin(); const output = pump(); assert.equal(M._terra_world_stream_close(task), 0); task = 0;
    assert.equal(M._tx_native_heap_used(), baseline);
    assert.deepEqual(output.subarray(output.readUInt32LE(34)), original.subarray(original.readUInt32LE(34)), 'untouched sections retain exact bytes');
    records[8] = 0; records[9] = 0; begin(); assert.equal(pump(true), null); assert.equal(M._terra_world_stream_close(task), 0); task = 0; records[9] = 1;
    assert.equal(M._tx_native_heap_used(), baseline);
    begin(); assert.equal(M._terra_world_stream_step(task, 1, ep), 0); assert.equal(M._terra_world_stream_cancel(task), 0); assert.equal(M._terra_world_stream_close(task), 0); task = 0;
    assert.deepEqual(d.sources.get(1), original); assert.equal(M._tx_native_heap_used(), baseline);
    d.open(output); assert.equal(d.cell(10, 10).type, 1); assert.equal(d.cell(10, 10).wires, 3); assert.equal(d.cell(109, 59).wires, 1); assert.equal(d.cell(110, 59).wires, 0);
    assert.equal(d.cell(11, 10).type, 314); assert.equal(d.cell(11, 10).fy, 65535, 'signed -1 back-track sentinel survives the raw word protocol');
  } finally { if (task) M._terra_world_stream_close(task); d.dispose(); for (const p of pointers) M._tx_free(p); }
});

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

test('world timers obey CheckMech shared 999-slot capacity and reclaim slots only during UpdateMech', async () => {
  const d = await makeDriver();
  try {
    const timers = Array.from({ length: 1000 }, (_, index) => ({
      x: index + 2, y: 4, type: 144, fx: 72, wires: index === 999 ? 1 : 0,
    }));
    d.open(makeCircuitWorld([...timers, { x: 1002, y: 4, type: 419, wires: 1 }], 1006, 8));
    for (const timer of timers) d.command(2, { x: timer.x, y: timer.y, flags: 1 });
    assert.equal(d.cell(1001, 4).fy, 18, 'HitSwitch still changes the frame when registration is full');
    d.command(3, { count: 15 });
    assert.equal(d.cell(1002, 4).fx, 0, 'the 1000th timer has no registered clock and cannot pulse');

    d.command(2, { x: 2, y: 4, flags: 1 });
    d.command(2, { x: 1001, y: 4, flags: 1 });
    d.command(2, { x: 1001, y: 4, flags: 1 });
    d.command(3, { count: 15 });
    assert.equal(d.cell(1002, 4).fx, 0, 'turning a timer off does not immediately free its coordinate slot');

    d.command(2, { x: 1001, y: 4, flags: 1 });
    d.command(2, { x: 1001, y: 4, flags: 1 });
    d.command(3, { count: 14 }); assert.equal(d.cell(1002, 4).fx, 0);
    d.command(3, { count: 1 }); assert.equal(d.cell(1002, 4).fx, 18, 'a later explicit activation can acquire the freed slot');
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
