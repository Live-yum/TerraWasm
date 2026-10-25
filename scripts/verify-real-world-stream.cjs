'use strict';

const fs = require('node:fs'),
  path = require('node:path'),
  assert = require('node:assert/strict'),
  crypto = require('node:crypto');
assert.ok(process.argv.length >= 4, 'Usage: node verify-real-world-stream.cjs <exact-producer-checkout> <world.wld> [more-worlds.wld]');
const root = path.resolve(process.argv[2]),
  files = process.argv.slice(3);
function independentHeader(fd, size) {
  const b = Buffer.alloc(65536);
  const n = fs.readSync(fd, b, 0, b.length, 0);
  const version = b.readUInt32LE(0);
  assert.ok(version >= 181);
  const count = b.readUInt16LE(24);
  assert.ok(count > 2 && count < 64);
  let p = b.readUInt32LE(26);
  assert.ok(p < n);
  const string = () => {
    let len = 0,
      shift = 0,
      v;
    do {
      v = b[p++];
      len |= (v & 127) << shift;
      shift += 7;
      assert.ok(shift <= 35);
    } while (v & 128);
    assert.ok(p + len <= n);
    const s = b.toString('utf8', p, p + len);
    p += len;
    return s;
  };
  const name = string(),
    seed = string();
  p += 8 + 16;
  const worldId = b.readInt32LE(p);
  p += 20;
  const height = b.readInt32LE(p),
    width = b.readInt32LE(p + 4);
  assert.ok(width > 0 && height > 0);
  return {
    version,
    name,
    seed,
    worldId,
    width,
    height,
    bytes: size
  };
}
async function main() {
  const results = [];
  for (const filename of files) {
    let fd,
      M,
      task = 0,
      world = 0;
    const pointers = [];
    try {
      fd = fs.openSync(filename, 'r');
      const size = fs.fstatSync(fd).size,
        header = independentHeader(fd, size),
        digest = crypto.createHash('sha256');
      const buf = Buffer.alloc(1048576);
      for (let o = 0; o < size;) {
        const n = fs.readSync(fd, buf, 0, Math.min(buf.length, size - o), o);
        assert.ok(n);
        digest.update(buf.subarray(0, n));
        o += n;
      }
      const hash = digest.digest('hex'),
        started = Date.now(),
        limit = 160 * 1024 * 1024;
      M = await require(path.join(root, 'build/terrax_world_wasm_web.js'))({
        wasmBinary: fs.readFileSync(path.join(root, 'build/terrax_world_wasm_web.wasm')),
        memoryGrowthLimit: () => limit
      });
      const initial = M.HEAPU8.byteLength,
        alloc = n => {
          const p = M._tx_malloc(n);
          assert.ok(p);
          pointers.push(p);
          return p;
        };
      const hp = alloc(4),
        ep = alloc(48),
        input = alloc(buf.length),
        required = alloc(8);
      let reads = 0,
        totalRead = 0,
        maxRead = 0,
        steps = 0,
        peakRss = process.memoryUsage().rss;
      const check = (s, label) => assert.equal(s, 0, label);
      const deadline = started + 120000;

      // Repeat admission/cancel/close before the successful full source scan.
      for (let i = 0; i < 3; i++) {
        check(M._terra_world_stream_open_begin(1, size, hp), 'cancel begin');
        task = M.HEAPU32[hp / 4];
        check(M._terra_world_stream_cancel(task), 'cancel');
        check(M._terra_world_stream_close(task), 'cancel close');
        task = 0;
      }
      check(M._terra_world_stream_open_begin(1, size, hp), 'open begin');
      task = M.HEAPU32[hp / 4];
      for (; steps < 1000000; steps++) {
        assert.ok(Date.now() < deadline, '120-second fixed stream deadline');
        check(M._terra_world_stream_step(task, 64, ep), 'step');
        const e = Array.from(M.HEAPU32.subarray(ep / 4, ep / 4 + 12));
        assert.equal(e[0], 1);
        if (e[1] === 4) break;
        if (e[1] === 0) continue;
        assert.equal(e[1], 1);
        assert.equal(e[2], 1);
        assert.ok(e[4] > 0 && e[4] <= buf.length);
        assert.ok(e[3] + e[4] <= size);
        // Replaying a suspended step must expose the identical request, not advance.
        check(M._terra_world_stream_step(task, 1, ep), 'replay step');
        assert.deepEqual(Array.from(M.HEAPU32.subarray(ep / 4, ep / 4 + 12)), e);
        assert.equal(fs.readSync(fd, buf, 0, e[4], e[3]), e[4]);
        M.HEAPU8.set(buf.subarray(0, e[4]), input);
        check(M._terra_world_stream_supply_source(task, 1, e[3], input, e[4]), 'supply');
        assert.equal(M._terra_world_stream_supply_source(task, 1, e[3], input, e[4]), -1, 'reject duplicated supply');
        reads++;
        totalRead += e[4];
        maxRead = Math.max(maxRead, e[4]);
        peakRss = Math.max(peakRss, process.memoryUsage().rss);
      }
      assert.ok(steps < 1000000);
      check(M._terra_world_stream_adopt(task, 1, hp), 'adopt');
      world = M.HEAPU32[hp / 4];
      check(M._terra_world_stream_close(task), 'task close');
      task = 0;
      check(M._terra_header_get_json(world, 0, 0n, required), 'header size');
      const needed = M.HEAPU32[required / 4];
      assert.equal(M.HEAPU32[required / 4 + 1], 0);
      assert.ok(needed <= 1048576);
      const output = alloc(needed);
      check(M._terra_header_get_json(world, output, BigInt(needed), required), 'header json');
      const raw = Buffer.from(M.HEAPU8.subarray(output, output + needed));
      const parsed = JSON.parse(raw.subarray(0, raw.indexOf(0) >= 0 ? raw.indexOf(0) : needed).toString());
      assert.equal(parsed.maxTilesX, header.width);
      assert.equal(parsed.maxTilesY, header.height);
      assert.equal(parsed.worldName, header.name);
      assert.ok(M.HEAPU8.byteLength <= limit);
      const heap = M.HEAPU8.byteLength;
      check(M._terra_world_close(world), 'world close');
      world = 0;
      for (const p of pointers) M._tx_free(p);
      pointers.length = 0;
      assert.equal(M._tx_native_heap_used(), 0, 'final native payload');
      assert.equal(M._tx_bridge_heap_used(), 0, 'final bridge payload');
      const report = {
        inputIndex: results.length,
        header: {
          version: header.version,
          width: header.width,
          height: header.height,
          bytes: header.bytes
        },
        sha256: hash,
        reads,
        totalRead,
        maxRead,
        steps,
        initialHeap: initial,
        finalHeap: heap,
        peakProcessRssObserved: peakRss,
        milliseconds: Date.now() - started,
        finalNativeBytes: 0,
        finalBridgeBytes: 0,
        scope: 'Web Wasm streaming open/cancel/replay/adopt/header/close, no Circuit execution, no full-file Buffer'
      };
      results.push(report);
      console.log(JSON.stringify(report));
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
        for (const pointer of pointers) cleanup(() => M._tx_free(pointer));
      }
      if (fd !== undefined) cleanup(() => fs.closeSync(fd));
      if (errors.length) throw new AggregateError(errors, 'verification cleanup failed');
    }
  }
  return results;
}
main().catch(e => {
  console.error(e);
  process.exitCode = 1;
});
