'use strict';
/* WLD-only acceptance. No TWLD input or WireHead pixel profile is attached.
 * The DUT is the Web Wasm physical wiring engine; the independent test-only
 * RISC-V decoder never supplies its state or memory-mapped display results. */
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

test('WLD-only original wiring executes the full physical RV32I CPU and round-trips its world', {
  skip: !process.env.TCW_WORLD ? 'Set TCW_WORLD to the pinned Computerraria WLD to run the full original-rule circuit DUT' : false,
  timeout: Number(process.env.TCW_TIMEOUT_MS || 60 * 60 * 1000),
}, async t => {
  const { makeRv32iProgram } = await import('./computerraria/rv32i-program.mjs');
  const { runReference } = await import('./computerraria/rv32i-golden.mjs');
  const { COMPUTERRARIA: C, romLampWrites, ramLampReads, ramLampWrites, decodeRamWords } = await import('./computerraria/mapping.mjs');
  const worldPath = path.resolve(process.env.TCW_WORLD);
  assert.equal(await sha256(worldPath), C.worldSha256, 'Use the pinned published world, not a reduced replacement');
  const buildDir = path.resolve(process.env.TCW_BUILD_DIR || path.join(__dirname, '../build'));
  const wasmPath = path.join(buildDir, 'terrax_world_wasm_web.wasm');
  const factory = require(path.join(buildDir, 'terrax_world_wasm_web.js'));
  const memoryLimit = Number(process.env.TCW_MEMORY_MIB || 160) * 1024 * 1024;
  const wasmBinary = fs.readFileSync(wasmPath);
  const M = await factory({ wasmBinary, memoryGrowthLimit: () => memoryLimit });
  const identityPointer = M._terra_build_info_json();
  assert.ok(identityPointer > 0 && identityPointer < M.HEAPU8.length, 'compiled identity pointer');
  const identityBytes = M.HEAPU8.subarray(identityPointer, Math.min(identityPointer + 65536, M.HEAPU8.length));
  const identityEnd = identityBytes.indexOf(0);
  assert.ok(identityEnd > 0, 'bounded compiled identity');
  const buildIdentity = JSON.parse(Buffer.from(identityBytes.subarray(0, identityEnd)).toString('utf8'));
  if (process.env.TCW_EXPECTED_COMMIT) {
    assert.equal(buildIdentity.sourceCommit, process.env.TCW_EXPECTED_COMMIT, 'test the artifact produced by this workflow');
    assert.equal(buildIdentity.dirty, false, 'CI acceptance requires a clean producer');
  }
  for (const method of ['begin', 'step', 'supply', 'ack', 'command', 'stats', 'cancel', 'close']) {
    assert.equal(typeof M['_terra_circuit_world_' + method], 'function', 'Missing actual circuit world ABI: ' + method);
  }
  assert.equal(M._terra_circuit_world_abi_version(), 1);
  const root = process.env.TMPDIR || path.resolve(__dirname, '../../.task');
  fs.mkdirSync(root, { recursive: true });
  const directory = fs.mkdtempSync(path.join(root, 'computerraria-wld-acceptance-'));
  const sources = new Map();
  function source(id, filename, writable = false) {
    if (sources.has(id)) fs.closeSync(sources.get(id).fd);
    const fd = fs.openSync(filename, writable ? 'w+' : 'r');
    sources.set(id, { fd, filename, writable });
    return fs.fstatSync(fd).size;
  }
  source(1, worldPath); source(2, path.join(directory, 'compiler-scratch.bin'), true);
  const pointers = [], alloc = size => {
    const p = M._tx_malloc(size); assert.ok(p, 'test bridge allocation'); pointers.push(p); return p;
  };
  const hp = alloc(4), ep = alloc(48), cp = alloc(64), sp = alloc(96), input = alloc(1024 * 1024);
  const errors = alloc(4096), errorSize = alloc(8);
  const hostInput = Buffer.allocUnsafe(1024 * 1024);
  let world = 0, circuit = 0, openTask = 0, lastProgress = 0;
  const start = Date.now(), deadline = start + Number(process.env.TCW_TIMEOUT_MS || 60 * 60 * 1000);
  const report = { executedAt: new Date(start).toISOString(), nodeVersion: process.version,
    sourceCommit: C.sourceCommit, worldSha256: C.worldSha256, mode: 'wld-only-original',
    wasmSha256: crypto.createHash('sha256').update(wasmBinary).digest('hex'),
    wasmBuild: { sourceCommit: buildIdentity.sourceCommit, dirty: buildIdentity.dirty,
      featureSet: buildIdentity.featureSet, target: buildIdentity.target, compiler: buildIdentity.compiler,
      initialMemory: buildIdentity.initialMemory, maxMemory: buildIdentity.maxMemory },
    sourceReadBytes: 0, sourceReadBytesById: {}, scratchWriteBytes: 0 };
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
  function pump(handle, opening = false) {
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
    return pump(circuit);
  }
  const trigger = (point, count = 1) => command(2, { ...point, count });
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
    t.diagnostic(JSON.stringify({ clocks, signature: signature.map(value => '0x' + value.toString(16)), ready: readReady(), after }));
    assert.equal(signature.at(-1), 0x600dc0de, `the physical CPU did not complete within ${clockBudget} clock inputs`);
    assert.ok(after.netPulses > before.netPulses && after.gatesFired > before.gatesFired, 'DUT must execute actual native networks and physical gates');
    return { signature, clocks, netPulses: after.netPulses - before.netPulses, gatesFired: after.gatesFired - before.gatesFired,
      elapsedMilliseconds: Date.now() - beganAt, after };
  }
  function openCircuit(worldSize) {
    const beganAt = Date.now();
    check(M._terra_world_stream_open_begin(1, worldSize, hp), 'stream open'); openTask = M.HEAPU32[hp >>> 2];
    pump(openTask, true);
    check(M._terra_world_stream_adopt(openTask, 1, hp), 'adopt immutable source'); world = M.HEAPU32[hp >>> 2];
    check(M._terra_world_stream_close(openTask), 'close open task'); openTask = 0;
    check(M._terra_circuit_world_begin(world, 2, 0, 0, memoryLimit, hp), 'WLD-only circuit import');
    circuit = M.HEAPU32[hp >>> 2];
    const ready = pump(circuit).event;
    assert.equal(ready[11] & 1, 0, 'WLD-only imports must retain original per-TripWire pixel rules');
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
  async function originalPixelRules() {
    const { makeCircuitWorld } = require('./helpers/circuit-world');
    const cells = new Map();
    const put = (x, y, tile = {}) => {
      const key = `${x},${y}`, old = cells.get(key) || {};
      cells.set(key, { ...old, ...tile, x, y, wires: (old.wires || 0) | (tile.wires || 0) });
    };
    for (let x = 2; x <= 6; x++) put(x, 4, { wires: 8 });
    for (const [x, wires] of [[4, 1], [6, 2]]) {
      put(x, 4, { type: 419, fx: 36, wires: 8 });
      put(x, 5, { type: 419, fx: 18 });
      put(x, 6, { type: 420, fx: 36, wires });
    }
    for (let y = 7; y <= 10; y++) put(4, y, { wires: 1 });
    for (let x = 5; x < 10; x++) put(x, 10, { wires: 1 });
    for (let x = 7; x <= 10; x++) put(x, 6, { wires: 2 });
    for (let y = 7; y < 10; y++) put(10, y, { wires: 2 });
    put(10, 10, { type: 445, wires: 3 });
    const bytes = makeCircuitWorld([...cells.values()]);
    const inputPath = path.join(directory, 'vanilla-pixel-rule.wld');
    const outputPath = path.join(directory, 'vanilla-pixel-rule-saved.wld');
    fs.writeFileSync(inputPath, bytes);
    source(1, inputPath); source(2, path.join(directory, 'pixel-rule-scratch.bin'), true);
    openCircuit(bytes.length);
    const frame = () => {
      const row = command(1, { x: 10, y: 10 }).rows[0];
      assert.equal(row[2] & 0xffff, 445); return row[3];
    };
    assert.equal(frame(), 0);
    const before = stats(); trigger({ x: 2, y: 4, mask: 8 });
    assert.equal(stats().gatesFired - before.gatesFired, 2, 'both real source gates fired in the same wave');
    assert.equal(frame(), 0, 'two independent gate TripWires do not combine their horizontal/vertical hits');
    const together = { x: 4, y: 6, width: 3, mask: 3 };
    trigger(together); assert.equal(frame(), 18, 'one TripWire that hits both axes toggles the pixel on');
    trigger({ x: 4, y: 6, mask: 1 }); trigger({ x: 6, y: 6, mask: 2 });
    assert.equal(frame(), 18, 'later separate axis pulses preserve the current pixel frame');
    trigger(together); assert.equal(frame(), 0, 'the next combined TripWire toggles the pixel off');
    trigger(together); assert.equal(frame(), 18);
    source(4, outputPath, true);
    const saved = command(6, { sourceId: 4 }).event;
    assert.equal(saved[9], 6); assert.equal(saved[11], 0, 'original-rule save needs only a WLD output');
    closeCircuit();
    source(1, outputPath); source(2, path.join(directory, 'pixel-rule-reimport.bin'), true);
    openCircuit(saved[10]); assert.equal(frame(), 18, 'WLD save/reopen preserves the lit original PixelBox');
    closeCircuit();
    report.originalPixelRules = { gateWaveSeparateTrips: 0, simultaneousAxes: 18,
      laterSeparateAxes: 18, secondSimultaneousAxes: 0, savedAndReopenedFrame: 18,
      sourceSha256: crypto.createHash('sha256').update(bytes).digest('hex') };
  }
  function monoProgram() {
    // Actual RV32I stores to the published monochrome buffer and update strobe.
    // No color-device address, framebuffer write API or software I/O emulator.
    const words = [0x00100fb7, 0x0020e0b7, 0x80000137, 0x00110113,
      0x0020a023, 0x00100113, 0x1e20ae23, 0x600dc1b7, 0x0de18193,
      0x0a3fae23, 0x0000006f];
    const bytes = new Uint8Array(words.length * 4), view = new DataView(bytes.buffer);
    words.forEach((word, i) => view.setUint32(i * 4, word, true)); return bytes;
  }
  try {
    await originalPixelRules();
    source(1, worldPath); source(2, path.join(directory, 'compiler-scratch.bin'), true);
    report.importMilliseconds = openCircuit(C.worldBytes);
    report.compiled = stats();
    assert.equal(report.compiled.width, C.width); assert.equal(report.compiled.height, C.height);
    assert.equal(report.compiled.wireCells, 72939714); assert.equal(report.compiled.gates, 13641575);
    // Network counts legitimately differ between vanilla and an attached mod's
    // junction/pixel profile. The published world's size and hash remain fixed.
    assert.ok(report.compiled.networks > 1000000, 'the complete wiring graph was compiled');
    const monoBefore = readMonoDisplay();
    assert.equal(monoBefore.length, 3072);
    assert.ok(monoBefore.every(row => (row[2] & 0xffff) === 445 && row[3] === 0));
    report.programBytes = program.bytes.byteLength;
    report.programSha256 = crypto.createHash('sha256').update(program.bytes).digest('hex');
    report.instructionKinds = program.instructionKinds; report.referenceRetired = reference.retired;
    const positive = execute(program.bytes);
    assert.deepEqual(positive.signature, program.checks.map(c => c.expected), 'physical RAM equals the independently checked RV32I golden');
    report.positive = positive;
    const modified = Uint8Array.from(program.bytes), view = new DataView(modified.buffer);
    const mutation = program.source.findIndex(line => line.startsWith('addi x3, x1,')); assert.ok(mutation >= 0);
    view.setUint32(mutation * 4, view.getUint32(mutation * 4, true) ^ 0x00100000, true);
    const negative = execute(modified);
    assert.equal(negative.signature[0], 0x7fffffff, 'an actual ROM bit change alters the physical arithmetic result');
    assert.deepEqual(negative.signature.slice(1), positive.signature.slice(1)); report.negative = negative;
    report.monoProgram = execute(monoProgram());
    const monoAfter = readMonoDisplay();
    assert.ok(monoAfter.every(row => (row[2] & 0xffff) === 445 && (row[3] === 0 || row[3] === 18)));
    report.monoDisplay = { cells: monoAfter.length, nonzeroFrames: monoAfter.filter(row => row[3] !== 0),
      observation: 'Original per-TripWire rules; the published mod world is not assumed to have vanilla-compatible display timing' };
    t.diagnostic(JSON.stringify({ monoDisplay: report.monoDisplay }));
    const savedWorld = path.join(directory, 'executed-original.wld');
    source(4, savedWorld, true);
    const saved = command(6, { sourceId: 4 }).event;
    assert.equal(saved[9], 6); assert.equal(saved[11], 0);
    assert.equal(fs.fstatSync(sources.get(4).fd).size, saved[10]);
    report.saved = { worldBytes: saved[10], worldSha256: await sha256(savedWorld) };
    closeCircuit();
    source(1, savedWorld); source(2, path.join(directory, 'reimport-scratch.bin'), true);
    report.reimportMilliseconds = openCircuit(saved[10]);
    assert.deepEqual(readMonoDisplay(), monoAfter, 'all 3072 original PixelBox frames survive WLD-only save/reopen');
    report.reimportCompiled = stats();
    report.systemBoundary = { ecall: probeSystemInstruction(0x00000073), ebreak: probeSystemInstruction(0x00100073) };
    report.reimported = execute(program.bytes);
    assert.deepEqual(report.reimported.signature, positive.signature, 'the saved WLD-only physical CPU still executes the complete RV32I image');
    closeCircuit();
    assert.equal(report.sourceReadBytesById[3], undefined, 'no TWLD source is registered or read, even if TCW_TWLD is set');
    report.elapsedMilliseconds = Date.now() - start; report.passed = true;
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
