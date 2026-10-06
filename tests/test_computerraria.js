'use strict';
/* The DUT below is the shipped Web Wasm circuit engine. JavaScript only feeds
 * files, patches physical ROM lamps, pulses a wire, and reads physical RAM lamps.
 * The independent RISC-V decoder is test-only and never supplies DUT state. */
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

async function sha256(file) {
  const hash = crypto.createHash('sha256');
  for await (const chunk of fs.createReadStream(file, { highWaterMark: 1024 * 1024 })) hash.update(chunk);
  return hash.digest('hex');
}

test('RV32I fixture has independently checked integer, branch, memory and FENCE signatures', async () => {
  const { makeRv32iProgram } = await import('./computerraria/rv32i-program.mjs');
  const { runReference } = await import('./computerraria/rv32i-golden.mjs');
  const program = makeRv32iProgram(), reference = runReference(program);
  assert.equal(program.instructionKinds.length, 38);
  assert.equal(program.checks.length, 48);
  assert.equal(reference.retired, 241);
  assert.equal(reference.signature.at(-1).actual, 0x600dc0de);
});

test('physical Computerraria memory mapping distinguishes new ROM and RAM bank pitches', async () => {
  const { COMPUTERRARIA: C, romBitCoordinate, ramBitCoordinate } = await import('./computerraria/mapping.mjs');
  assert.deepEqual(romBitCoordinate(0, 0), { x: 2853, y: 1236 });
  assert.deepEqual(romBitCoordinate(4, 31), { x: 2855, y: 1143 });
  assert.deepEqual(romBitCoordinate(8, 31), { x: 2856, y: 1143 });
  assert.deepEqual(romBitCoordinate(32768, 31), { x: 2853, y: 1274 });
  assert.deepEqual(ramBitCoordinate(C.ramBase, 0), { x: 2853, y: 4380 });
  assert.deepEqual(ramBitCoordinate(C.ramBase + 16384, 31), { x: 2853, y: 4412 });
  assert.deepEqual(ramBitCoordinate(C.ramBase + C.ramBytes - 4, 0, 1), { x: 15139, y: 7130 });
  assert.throws(() => romBitCoordinate(C.romBytes, 0), RangeError);
  assert.throws(() => ramBitCoordinate(0x11110, 0), RangeError);
});

test('actual full-world Wasm circuit executes the RV32I program and a ROM mutation negative control', {
  skip: !process.env.TCW_WORLD ? 'Set TCW_WORLD and TCW_TWLD to the pinned actual Computerraria files to run the circuit DUT' : false,
  timeout: Number(process.env.TCW_TIMEOUT_MS || 60 * 60 * 1000),
}, async t => {
  const { makeRv32iProgram } = await import('./computerraria/rv32i-program.mjs');
  const { runReference } = await import('./computerraria/rv32i-golden.mjs');
  const { COMPUTERRARIA: C, romLampWrites, ramLampReads, ramLampWrites, decodeRamWords } = await import('./computerraria/mapping.mjs');
  const worldPath = path.resolve(process.env.TCW_WORLD);
  assert.ok(process.env.TCW_TWLD, 'Actual acceptance includes the .twld colored display and preserved mod metadata');
  const twldPath = path.resolve(process.env.TCW_TWLD);
  assert.equal(await sha256(worldPath), C.worldSha256, 'Use the pinned published world, not a reduced replacement');
  assert.equal(await sha256(twldPath), C.twldSha256);
  const buildDir = path.resolve(process.env.TCW_BUILD_DIR || path.join(__dirname, '../build'));
  const wasmPath = path.join(buildDir, 'terrax_world_wasm_web.wasm');
  const factory = require(path.join(buildDir, 'terrax_world_wasm_web.js'));
  const memoryLimit = Number(process.env.TCW_MEMORY_MIB || 160) * 1024 * 1024;
  const M = await factory({ wasmBinary: fs.readFileSync(wasmPath), memoryGrowthLimit: () => memoryLimit });
  for (const method of ['begin', 'step', 'supply', 'ack', 'command', 'stats', 'cancel', 'close']) {
    assert.equal(typeof M['_terra_circuit_world_' + method], 'function', 'Missing actual circuit world ABI: ' + method);
  }
  assert.equal(M._terra_circuit_world_abi_version(), 1);
  const root = process.env.TMPDIR || path.resolve(__dirname, '../../.task');
  fs.mkdirSync(root, { recursive: true });
  const directory = fs.mkdtempSync(path.join(root, 'computerraria-acceptance-'));
  const sources = new Map();
  function source(id, filename, writable = false) {
    if (sources.has(id)) fs.closeSync(sources.get(id).fd);
    const fd = fs.openSync(filename, writable ? 'w+' : 'r');
    sources.set(id, { fd, filename, writable });
    return fs.fstatSync(fd).size;
  }
  source(1, worldPath); source(2, path.join(directory, 'compiler-scratch.bin'), true);
  const twldSize = source(3, twldPath);
  const pointers = [], alloc = size => {
    const p = M._tx_malloc(size); assert.ok(p, 'test bridge allocation'); pointers.push(p); return p;
  };
  const hp = alloc(4), ep = alloc(48), cp = alloc(64), sp = alloc(96), input = alloc(1024 * 1024);
  const errors = alloc(4096), errorSize = alloc(8);
  const hostInput = Buffer.allocUnsafe(1024 * 1024);
  let world = 0, circuit = 0, openTask = 0, lastProgress = 0;
  const start = Date.now(), deadline = start + Number(process.env.TCW_TIMEOUT_MS || 60 * 60 * 1000);
  const report = { executedAt: new Date(start).toISOString(), nodeVersion: process.version,
    sourceCommit: C.sourceCommit, worldSha256: C.worldSha256, twldSha256: C.twldSha256,
    wasmSha256: await sha256(wasmPath), directory, sourceReadBytes: 0, sourceReadBytesById: {}, scratchWriteBytes: 0 };
  function check(status, operation) {
    if (status === 0 || status === 1) return;
    let detail = '';
    if (typeof M._terra_info_get_last_error_json === 'function') {
      M._terra_info_get_last_error_json(errors, 4096n, errorSize);
      const bytes = M.HEAPU8.subarray(errors, errors + 4096);
      const zero = bytes.indexOf(0); detail = Buffer.from(zero < 0 ? bytes : bytes.subarray(0, zero)).toString();
    }
    assert.equal(status, 0, `${operation}: ${detail}`);
  }
  function stats() {
    check(M._terra_circuit_world_stats(circuit, sp), 'circuit stats');
    const s = Array.from(M.HEAPU32.subarray(sp >>> 2, (sp >>> 2) + 24));
    return { width: s[2], height: s[3], wireCells: s[10], devices: s[11], gates: s[12], networks: s[13],
      phase: s[15], activeBytes: s[16], peakBytes: s[17], ticks: s[18] + s[19] * 2 ** 32,
      netPulses: s[20] + s[21] * 2 ** 32, gatesFired: s[22] + s[23] * 2 ** 32, wasmHeapBytes: M.HEAPU8.byteLength };
  }
  function readRequest(e, supply, handle) {
    const file = sources.get(e[2]); assert.ok(file, 'source ID is registered');
    assert.ok(e[4] > 0 && e[4] <= hostInput.length, 'bounded source request');
    const read = fs.readSync(file.fd, hostInput, 0, e[4], e[3]); assert.equal(read, e[4]);
    M.HEAPU8.set(hostInput.subarray(0, read), input);
    check(supply(handle, e[2], e[3], input, read), 'supply source');
    report.sourceReadBytes += read;
    report.sourceReadBytesById[e[2]] = (report.sourceReadBytesById[e[2]] || 0) + read;
  }
  function pump(handle, opening = false, pauseAtOutputSource = 0) {
    const rows = [];
    for (;;) {
      assert.ok(Date.now() < deadline, 'native circuit acceptance deadline');
      check(opening ? M._terra_world_stream_step(handle, 65536, ep) : M._terra_circuit_world_step(handle, 65536, ep), 'native step');
      const e = Array.from(M.HEAPU32.subarray(ep >>> 2, (ep >>> 2) + 12));
      assert.equal(e[0], 1);
      if (e[1] === 1) readRequest(e, opening ? M._terra_world_stream_supply_source : M._terra_circuit_world_supply, handle);
      else if ((!opening && e[1] === 2) || (opening && e[1] === 3)) {
        assert.ok(!opening, 'opening the immutable source produces no output file');
        const file = sources.get(e[2]); assert.ok(file?.writable);
        assert.ok(e[4] <= 1024 * 1024);
        let written = 0;
        while (written < e[4]) written += fs.writeSync(file.fd, M.HEAPU8, e[5] + written, e[4] - written, e[3] + written);
        report.scratchWriteBytes += written;
        check(M._terra_circuit_world_ack(handle), 'ack output');
        if (e[2] === pauseAtOutputSource) return { rows, event: e, paused: true };
      } else if (!opening && e[1] === 3) {
        assert.equal(e[4], e[10] * 16);
        const values = M.HEAPU32.subarray(e[5] >>> 2, (e[5] >>> 2) + e[10] * 4);
        for (let i = 0; i < values.length; i += 4) rows.push(Array.from(values.subarray(i, i + 4)));
        check(M._terra_circuit_world_ack(handle), 'ack result');
      } else if (e[1] === 4) return { rows, event: e };
      else assert.equal(e[1], 0, 'known event kind');
      if (Date.now() - lastProgress > 15000) {
        lastProgress = Date.now();
        t.diagnostic(JSON.stringify({ seconds: (Date.now() - start) / 1000, ...(circuit ? stats() : { opening: true }) }));
      }
    }
  }
  function command(kind, options = {}) {
    let data = 0;
    if (options.records?.length) {
      data = M._tx_malloc(options.records.length * 16); assert.ok(data);
      for (let i = 0; i < options.records.length; i++) {
        const r = options.records[i]; M.HEAPU32.set([r.x, r.y, r.value || 0, 0], (data >>> 2) + i * 4);
      }
    }
    try {
      M.HEAPU32.set([1, kind, options.x || 0, options.y || 0, options.width || 1, options.height || 1,
        options.stride || 1, options.mask || 0, options.count || 0, data, options.records?.length || 0,
        options.sourceId || 0, options.flags || 0, options.auxSourceId || 0, 0, 0], cp >>> 2);
      check(M._terra_circuit_world_command(circuit, cp), 'command ' + kind);
    } finally { if (data) M._tx_free(data); }
    return pump(circuit, false, options.pauseAtOutputSource || 0);
  }
  const trigger = (point, count = 1) => command(2, { ...point, count });
  const readColorDisplay = () => command(1, { x: 7371, y: 1002, width: 176, height: 96 }).rows;
  const readMonoDisplay = () => command(1, { x: 6485, y: 800, width: 64, height: 48 }).rows;
  function readReady() {
    const rows = command(4, { records: [{ ...C.readyLamp, value: 0 }] }).rows;
    assert.equal(rows.length, 1); assert.equal(rows[0][3], 419); return rows[0][2] === 1;
  }
  function reset() {
    for (let tries = 0; !readReady() && tries < 3; tries++) trigger(C.clock);
    if (!readReady()) trigger(C.reset);
    trigger(C.zeroDataBus); trigger(C.zeroMemorySelect); trigger(C.storePc);
  }
  const program = makeRv32iProgram(), reference = runReference(program);
  const addresses = program.checks.map(check => check.address);
  function readSignature() {
    // The RAM latches share data and select buses. Raw lamp states encode their
    // current parity until these two physical controls return the buses to zero.
    // The upstream tinterface uses the same zdb + zmem sequence before read_bin.
    trigger(C.zeroDataBus);
    trigger(C.zeroMemorySelect);
    return decodeRamWords(command(4, { records: ramLampReads(addresses) }).rows, addresses);
  }
  function execute(bytes) {
    const beganAt = Date.now();
    reset();
    command(5, { records: romLampWrites(bytes) });
    command(5, { records: ramLampWrites(addresses.map(address => ({ address, value: 0 }))) });
    reset();
    const before = stats();
    let signature, clocks = 0;
    const clockBudget = Number(process.env.TCW_CLOCK_BUDGET || 4096);
    while (clocks < clockBudget) {
      const batch = Math.min(128, clockBudget - clocks);
      trigger(C.clock, batch); clocks += batch;
      for (let finish = 0; !readReady() && finish < 3; finish++) {
        trigger(C.clock); clocks++;
      }
      assert.ok(readReady(), 'RAM inspection occurs only at an instruction boundary');
      signature = readSignature();
      if (signature.at(-1) === 0x600dc0de) break;
    }
    const after = stats();
    report.lastExecution = { signature, clocks, netPulses: after.netPulses - before.netPulses, gatesFired: after.gatesFired - before.gatesFired, after };
    t.diagnostic(JSON.stringify({ clocks, signature: signature.map(value => '0x' + value.toString(16)), ready: readReady(), after }));
    assert.equal(signature.at(-1), 0x600dc0de, `the physical CPU did not complete within ${clockBudget} clock inputs`);
    assert.ok(after.netPulses > before.netPulses && after.gatesFired > before.gatesFired, 'DUT must execute actual native networks and physical gates');
    return { signature, clocks, netPulses: after.netPulses - before.netPulses, gatesFired: after.gatesFired - before.gatesFired,
      elapsedMilliseconds: Date.now() - beganAt, after };
  }
  function openCircuit(worldSize, sidecarSize) {
    const beganAt = Date.now();
    check(M._terra_world_stream_open_begin(1, worldSize, hp), 'stream open'); openTask = M.HEAPU32[hp >>> 2];
    pump(openTask, true);
    check(M._terra_world_stream_adopt(openTask, 1, hp), 'adopt immutable source'); world = M.HEAPU32[hp >>> 2];
    check(M._terra_world_stream_close(openTask), 'close open task'); openTask = 0;
    check(M._terra_circuit_world_begin(world, 2, 3, sidecarSize, memoryLimit, hp), 'circuit import');
    circuit = M.HEAPU32[hp >>> 2]; pump(circuit);
    return Date.now() - beganAt;
  }
  function closeCircuit() {
    if (circuit) { check(M._terra_circuit_world_close(circuit), 'close circuit'); circuit = 0; }
    if (world) { check(M._terra_world_close(world), 'close world'); world = 0; }
    assert.equal(M._tx_native_heap_used(), 0, 'native owners release the complete full-world graph');
  }
  function probeSystemInstruction(opcode) {
    // The published circuit has no SYSTEM-opcode microprogram or trap CSR bank.
    // Measure its boundary using the same physical ROM/clock/RAM interfaces.
    const bytes = new Uint8Array(24), view = new DataView(bytes.buffer);
    [0x00100fb7, opcode, 0x600dc1b7, 0x0de18193, 0x0a3fae23, 0x0000006f].forEach((word, i) => view.setUint32(i * 4, word, true));
    reset(); command(5, { records: romLampWrites(bytes) });
    command(5, { records: ramLampWrites([{ address: addresses.at(-1), value: 0 }]) }); reset();
    trigger(C.clock, 16);
    const ready = readReady();
    reset();
    const completion = readSignature().at(-1);
    assert.equal(ready, false, 'unsupported SYSTEM instruction does not retire as a silently accepted NOP');
    assert.equal(completion, 0, 'a program after the unsupported SYSTEM instruction did not execute');
    return { opcode, inputClocks: 16, ready, completion };
  }
  try {
    report.importMilliseconds = openCircuit(C.worldBytes, twldSize);
    report.compiled = stats();
    assert.equal(report.compiled.width, C.width); assert.equal(report.compiled.height, C.height);
    assert.equal(report.compiled.wireCells, 72939714); assert.equal(report.compiled.gates, 13641575);
    assert.ok(report.compiled.networks > 1000000, 'full compiled world, without reducing the circuit fixture');
    assert.ok(report.sourceReadBytesById[3] >= twldSize * 2, 'native metadata and tile replay must actually read the TWLD sidecar');
    const displayBefore = readColorDisplay();
    assert.equal(displayBefore.length, 16896);
    assert.ok(displayBefore.every(row => (row[2] & 0xffff) === 0xfffe && (row[2] & 0x80000)), 'all actual ColorPixelBox cells are restored from TWLD');
    assert.ok(displayBefore.every(row => row[3] === 0), 'the pinned colored display begins blank');
    report.colorPixels = displayBefore.length;
    report.programBytes = program.bytes.byteLength;
    report.programSha256 = crypto.createHash('sha256').update(program.bytes).digest('hex');
    report.instructionKinds = program.instructionKinds; report.referenceRetired = reference.retired;
    const positive = execute(program.bytes);
    assert.deepEqual(positive.signature, program.checks.map(c => c.expected), 'physical RAM signature equals independent RV32I golden');
    report.positive = positive;
    const modified = Uint8Array.from(program.bytes), view = new DataView(modified.buffer);
    const mutation = program.source.findIndex(line => line.startsWith('addi x3, x1,')); assert.ok(mutation >= 0);
    view.setUint32(mutation * 4, view.getUint32(mutation * 4, true) ^ 0x00100000, true);
    const negative = execute(modified);
    assert.equal(negative.signature[0], 0x7fffffff, 'changing one actual ROM instruction changes the arithmetic result');
    assert.deepEqual(negative.signature.slice(1), positive.signature.slice(1));
    report.negative = negative;
    const { makeDisplayProgram } = await import('./computerraria/io-program.mjs');
    report.displayProgram = execute(makeDisplayProgram().bytes);
    const colorAfter = readColorDisplay(), monoAfter = readMonoDisplay();
    const colored = colorAfter.filter(row => row[3] !== 0), monochrome = monoAfter.filter(row => row[3] !== 0);
    report.display = { colored, monochrome };
    t.diagnostic(JSON.stringify({ display: report.display }));
    assert.equal(colored.length, 8, 'a physical CPU store changes eight packed four-bit pixels');
    assert.ok(colored.every(row => row[3] === ((54 << 16) | 54)), 'all four native ColorPixelBox channel bits are set');
    assert.equal(monochrome.length, 2, 'the monochrome screen receives both set bits from SW');
    assert.ok(monochrome.every(row => (row[2] & 0xffff) === 445 && row[3] === 18));
    const savedWorld = path.join(directory, 'executed.wld'), savedTwld = path.join(directory, 'executed.twld');
    source(4, savedWorld, true); source(5, savedTwld, true);
    const sidecarReadBeforeCancel = report.sourceReadBytesById[3];
    const interrupted = command(6, { sourceId: 4, auxSourceId: 5, pauseAtOutputSource: 5 });
    assert.equal(interrupted.paused, true, 'the real paired save reaches TWLD gzip output before cancellation');
    const replayBytesRead = report.sourceReadBytesById[3] - sidecarReadBeforeCancel;
    assert.ok(replayBytesRead > 0 && replayBytesRead < twldSize, 'the sidecar replay is still incomplete');
    report.saveCancellation = { replayBytesRead, stagedSidecarBytes: fs.fstatSync(sources.get(5).fd).size };
    check(M._terra_circuit_world_cancel(circuit), 'cancel the incomplete real TWLD output replay');
    assert.deepEqual(readColorDisplay(), colorAfter, 'cancelled save keeps all live ColorPixelBox frames');
    assert.deepEqual(readMonoDisplay(), monoAfter, 'cancelled save keeps all live mono pixel frames');
    report.saveCancellation.sessionRemainsReadable = true;
    source(4, savedWorld, true); source(5, savedTwld, true); // Discard both uncommitted staging files.
    const saved = command(6, { sourceId: 4, auxSourceId: 5 }).event;
    assert.equal(saved[9], 6);
    assert.equal(fs.fstatSync(sources.get(4).fd).size, saved[10]);
    assert.equal(fs.fstatSync(sources.get(5).fd).size, saved[11]);
    report.saved = { worldBytes: saved[10], twldBytes: saved[11], worldSha256: await sha256(savedWorld), twldSha256: await sha256(savedTwld) };
    closeCircuit();
    source(1, savedWorld); source(2, path.join(directory, 'reimport-scratch.bin'), true); source(3, savedTwld);
    report.reimportMilliseconds = openCircuit(saved[10], saved[11]);
    assert.deepEqual(readColorDisplay(), colorAfter, 'all ColorPixelBox cells retain their actual rendered state after WLD/TWLD save and reimport');
    assert.deepEqual(readMonoDisplay(), monoAfter, 'all vanilla pixel cells retain their actual rendered state after WLD save and reimport');
    report.reimportCompiled = stats();
    report.clearDisplayProgram = execute(makeDisplayProgram(0, 0).bytes);
    assert.ok(readColorDisplay().every(row => row[3] === 0), 'the reimported physical screen controller can clear its previously written color word');
    assert.ok(readMonoDisplay().every(row => row[3] === 0), 'the reimported physical screen controller can clear its previously written mono word');
    report.systemBoundary = { ecall: probeSystemInstruction(0x00000073), ebreak: probeSystemInstruction(0x00100073) };
    const reimported = execute(program.bytes);
    assert.deepEqual(reimported.signature, positive.signature, 'the saved and reimported circuit still executes the complete RV32I program');
    report.reimported = reimported;
    closeCircuit();
    report.elapsedMilliseconds = Date.now() - start;
    report.passed = true;
    t.diagnostic(JSON.stringify(report));
  } finally {
    if (circuit) M._terra_circuit_world_close(circuit);
    if (openTask) M._terra_world_stream_close(openTask);
    if (world) M._terra_world_close(world);
    for (const pointer of pointers) M._tx_free(pointer);
    for (const file of sources.values()) fs.closeSync(file.fd);
    if (process.env.TCW_REPORT) fs.writeFileSync(process.env.TCW_REPORT, JSON.stringify(report, null, 2) + '\n');
    if (!process.env.TCW_KEEP_SCRATCH) fs.rmSync(directory, { recursive: true, force: true });
  }
});
