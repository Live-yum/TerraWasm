'use strict';

const fs = require('node:fs'),
  path = require('node:path'),
  os = require('node:os'),
  assert = require('node:assert/strict'),
  crypto = require('node:crypto');
assert.ok(process.argv.length >= 4, 'Usage: node verify-real-world-save.cjs <exact-producer-checkout> <world.wld> [more-worlds.wld]');
const root = path.resolve(process.argv[2]),
  files = process.argv.slice(3),
  MiB = 1048576;
function hashFile(fd, size, buffer) {
  const h = crypto.createHash('sha256');
  for (let o = 0; o < size;) {
    const n = fs.readSync(fd, buffer, 0, Math.min(buffer.length, size - o), o);
    assert.ok(n);
    h.update(buffer.subarray(0, n));
    o += n;
  }
  return h.digest('hex');
}
(async () => {
  for (const file of files) {
    let dir,
      outputPath,
      source,
      output,
      M,
      task = 0,
      world = 0;
    const ptrs = [];
    try {
      dir = fs.mkdtempSync(path.join(os.tmpdir(), 'terra-real-save-'));
      outputPath = path.join(dir, 'output.wld');
      source = fs.openSync(file, 'r');
      const size = fs.fstatSync(source).size;
      output = fs.openSync(outputPath, 'wx+');
      const buffer = Buffer.alloc(MiB),
        before = hashFile(source, size, buffer);
      const factory = require(path.join(root, 'build/terrax_world_wasm_web.js'));
      M = await factory({
        wasmBinary: fs.readFileSync(path.join(root, 'build/terrax_world_wasm_web.wasm')),
        memoryGrowthLimit: () => 160 * MiB
      });
      const started = Date.now(),
        deadline = started + 120000,
        alloc = n => {
          const p = M._tx_malloc(n);
          assert.ok(p);
          ptrs.push(p);
          return p;
        },
        str = s => {
          const b = Buffer.from(s + '\0'),
            p = alloc(b.length);
          M.HEAPU8.set(b, p);
          return p;
        };
      const hp = alloc(4),
        ep = alloc(48),
        input = alloc(MiB),
        required = alloc(8),
        name = str('save'),
        request = str('{}'),
        sources = new Map([[1, {
          fd: source,
          size
        }]]);
      let totalRead = 0,
        totalWrite = 0,
        maxRead = 0,
        maxWrite = 0,
        sourceRequests = 0,
        outputEvents = 0,
        peakHeap = M.HEAPU8.length;
      function check(s, label) {
        assert.equal(s, 0, label);
      }
      function pump(id, allowOutput) {
        for (let i = 0; i < 1000000; i++) {
          assert.ok(Date.now() < deadline, 'fixed120s overall deadline');
          check(M._terra_world_stream_step(id, 64, ep), 'step');
          const e = Array.from(M.HEAPU32.subarray(ep / 4, ep / 4 + 12));
          assert.equal(e[0], 1);
          peakHeap = Math.max(peakHeap, M.HEAPU8.length);
          assert.ok(peakHeap <= 160 * MiB);
          if (e[1] === 0) continue;
          if (e[1] === 4) return {
            size: e[10],
            kind: e[11]
          };
          if (e[1] === 1) {
            const s = sources.get(e[2]);
            assert.ok(s);
            assert.ok(e[4] > 0 && e[4] <= MiB && e[3] + e[4] <= s.size);
            assert.equal(fs.readSync(s.fd, buffer, 0, e[4], e[3]), e[4]);
            M.HEAPU8.set(buffer.subarray(0, e[4]), input);
            check(M._terra_world_stream_supply_source(id, e[2], e[3], input, e[4]), 'supply');
            totalRead += e[4];
            sourceRequests++;
            maxRead = Math.max(maxRead, e[4]);
          } else if (e[1] === 3) {
            assert.ok(allowOutput);
            assert.ok(e[4] > 0 && e[4] <= MiB);
            assert.ok(e[3] + e[4] <= size + MiB, 'fixed bounded output disk reservation');
            const view = M.HEAPU8.subarray(e[5], e[5] + e[4]);
            assert.equal(fs.writeSync(output, view, 0, view.length, e[3]), view.length);
            totalWrite += view.length;
            maxWrite = Math.max(maxWrite, view.length);
            outputEvents++;
            check(M._terra_world_stream_ack_output(id), 'ack');
          } else assert.fail('unexpected event' + e[1]);
        }
        assert.fail('fixed1,000,000-step bound');
      }
      function header() {
        check(M._terra_header_get_json(world, 0, 0n, required), 'header size');
        const n = M.HEAPU32[required / 4];
        assert.equal(M.HEAPU32[required / 4 + 1], 0);
        assert.ok(n <= MiB);
        const p = alloc(n);
        check(M._terra_header_get_json(world, p, BigInt(n), required), 'header');
        const bytes = Buffer.from(M.HEAPU8.subarray(p, p + n));
        return JSON.parse(bytes.subarray(0, bytes.indexOf(0) >= 0 ? bytes.indexOf(0) : bytes.length).toString());
      }
      check(M._terra_world_stream_open_begin(1, size, hp), 'initial begin');
      task = M.HEAPU32[hp / 4];
      pump(task, false);
      check(M._terra_world_stream_adopt(task, 1, hp), 'initial adopt');
      world = M.HEAPU32[hp / 4];
      check(M._terra_world_stream_close(task), 'initial task close');
      task = 0;
      const firstHeader = header();
      check(M._terra_world_stream_operation_begin(world, name, request, hp), 'save begin');
      task = M.HEAPU32[hp / 4];
      const saved = pump(task, true);
      assert.equal(saved.kind, 0);
      assert.ok(saved.size > 0 && saved.size <= size + MiB);
      fs.ftruncateSync(output, saved.size);
      fs.fsyncSync(output);
      sources.set(2, {
        fd: output,
        size: saved.size
      });
      check(M._terra_world_stream_adopt(task, 2, hp), 'save adopt');
      world = M.HEAPU32[hp / 4];
      check(M._terra_world_stream_close(task), 'save task close');
      task = 0;
      assert.deepEqual(header(), firstHeader, 'saved adopted header');
      check(M._terra_world_close(world), 'saved world close');
      world = 0;
      check(M._terra_world_stream_open_begin(2, saved.size, hp), 'reopen begin');
      task = M.HEAPU32[hp / 4];
      pump(task, false);
      check(M._terra_world_stream_adopt(task, 2, hp), 'reopen adopt');
      world = M.HEAPU32[hp / 4];
      check(M._terra_world_stream_close(task), 'reopen task close');
      task = 0;
      assert.deepEqual(header(), firstHeader, 'reopened complete header');
      check(M._terra_world_close(world), 'reopened close');
      world = 0;
      const outputHash = hashFile(output, saved.size, buffer),
        after = hashFile(source, size, buffer);
      assert.equal(after, before, 'original input unchanged');
      assert.equal(saved.size, size, 'clean save exact size');
      assert.equal(outputHash, before, 'clean save byte identity');
      for (const p of ptrs) M._tx_free(p);
      ptrs.length = 0;
      assert.equal(M._tx_native_heap_used(), 0);
      assert.equal(M._tx_bridge_heap_used(), 0);
      console.log(JSON.stringify({
        inputIndex: files.indexOf(file),
        inputBytes: size,
        outputBytes: saved.size,
        inputSha256: before,
        outputSha256: outputHash,
        dimensions: {
          width: firstHeader.maxTilesX,
          height: firstHeader.maxTilesY
        },
        sourceRequests,
        outputEvents,
        totalRead,
        totalWrite,
        maxRead,
        maxWrite,
        peakWasmHeap: peakHeap,
        heapCap: 160 * MiB,
        milliseconds: Date.now() - started,
        finalNativeBytes: 0,
        finalBridgeBytes: 0,
        scope: 'nonCircuit streamed clean save/adopt/close/reopen/header and byte-identical full-file SHA, Node-hosted Web Wasm'
      }));
    } finally {
      const errors = [];
      const cleanup = fn => {
        try {
          fn();
        } catch (error) {
          errors.push(error);
        }
      };
      if (M) {
        if (task) cleanup(() => assert.equal(M._terra_world_stream_close(task), 0, 'cleanup task close'));
        if (world) cleanup(() => assert.equal(M._terra_world_close(world), 0, 'cleanup world close'));
        for (const pointer of ptrs) cleanup(() => M._tx_free(pointer));
      }
      if (source !== undefined) cleanup(() => fs.closeSync(source));
      if (output !== undefined) cleanup(() => fs.closeSync(output));
      if (outputPath && fs.existsSync(outputPath)) cleanup(() => fs.unlinkSync(outputPath));
      if (dir) cleanup(() => fs.rmdirSync(dir));
      if (errors.length) throw new AggregateError(errors, 'verification cleanup failed');
    }
  }
})().catch(e => {
  console.error(e);
  process.exitCode = 1;
});
