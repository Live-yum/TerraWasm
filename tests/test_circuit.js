'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { performance } = require('node:perf_hooks');
const ROOT = path.resolve(__dirname, '..');
const BUILD = path.resolve(process.env.TERRA_CIRCUIT_BUILD_DIR || path.join(ROOT, 'build'));
const FLAGS = { tile: 1, seed: 2, horizontal: 4, vertical: 8 };

async function moduleFor(target) {
  const name = target === 'node' ? 'terrax_world_wasm' : 'terrax_world_wasm_web';
  return require(path.join(BUILD, `${name}.js`))({
    wasmBinary: fs.readFileSync(path.join(BUILD, `${name}.wasm`)),
  });
}
class Bridge {
  constructor(M, width, height, capacity, maxBytes = 96 * 1024 * 1024) {
    this.M = M; this.handles = []; this.buffers = [];
    this.input = this.alloc(65536 * 16); this.output = this.alloc(65536 * 16);
    this.result = this.alloc(64);
    assert.equal(M._terra_circuit_create(width, height, capacity, maxBytes, this.result), 0);
    this.handle = this.read(this.result, 1)[0];
  }
  alloc(bytes) { const p = this.M._tx_malloc(bytes); assert.ok(p); this.buffers.push(p); return p; }
  read(p, count) { return Array.from(this.M.HEAPU32.subarray(p / 4, p / 4 + count)); }
  write(values) { this.M.HEAPU32.set(values, this.input / 4); }
  load(rows) {
    for (let i = 0; i < rows.length; i += 65536) {
      const chunk = rows.slice(i, i + 65536); this.write(chunk.flat());
      assert.equal(this.M._terra_circuit_load(this.handle, this.input, chunk.length), 0);
    }
  }
  compile(chunk = 65536) {
    let status;
    do { status = this.M._terra_circuit_compile(this.handle, chunk, this.result); assert.ok(status >= 0); } while (status === 1);
  }
  begin(seeds, colour = 0, trace = true, limit = 1000000) {
    this.write(seeds.flat());
    return this.M._terra_circuit_begin(this.handle, this.input, seeds.length, colour, trace ? 1 : 0, limit);
  }
  step(chunk = 65536, capacity = 65536) {
    const status = this.M._terra_circuit_step(this.handle, chunk, this.output, capacity, this.result);
    const [processed, emitted, remaining, totalProcessed] = this.read(this.result, 4);
    const events = []; if (status >= 0) for (let i = 0; i < emitted; i++) events.push(this.read(this.output + i * 16, 4));
    return { status, processed, events, remaining, totalProcessed };
  }
  patch(rows) { this.write(rows.flat()); return this.M._terra_circuit_patch(this.handle, this.input, rows.length); }
  stats() { assert.equal(this.M._terra_circuit_stats(this.handle, this.result), 0); return this.read(this.result, 16); }
  close() {
    assert.equal(this.M._terra_circuit_close(this.handle), 0);
    for (const p of this.buffers.reverse()) this.M._tx_free(p);
  }
}

function reference(rows, width, height, seeds, colour, limit) {
  const wires = new Map(rows.map(r => [`${r[0]},${r[1]}`, r]));
  const queue = [], counters = new Map(), skipped = new Set(), events = [];
  const dirs = [[0,1],[0,-1],[1,0],[-1,0]], routes = [[0,1,2,3],[3,2,1,0],[2,3,0,1]];
  for (const [x,y] of seeds) {
    const key = `${x},${y}`;
    if (!(wires.get(key)?.[2] & (1 << colour)) || skipped.has(key)) continue;
    skipped.add(key); counters.set(key, 4); queue.push([x,y,0]);
  }
  for (let cursor = 0; cursor < queue.length; cursor++) {
    if (cursor === limit) return { status: -4, events };
    const [x,y,incoming] = queue[cursor], key = `${x},${y}`, routing = wires.get(key)[3];
    let flags = (routing ? FLAGS.tile : 0) | (skipped.has(key) ? FLAGS.seed : 0);
    for (let d = 0; d < 4; d++) {
      const nx = x + dirs[d][0], ny = y + dirs[d][1];
      if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
      if (routing >= 2 && routing <= 4 && routes[routing - 2][incoming] !== d) continue;
      if (routing === 5) { if (incoming !== d) continue; flags |= d < 2 ? FLAGS.vertical : FLAGS.horizontal; }
      const nk = `${nx},${ny}`, next = wires.get(nk);
      if (!(next?.[2] & (1 << colour))) continue;
      if (counters.has(nk)) { const n = counters.get(nk) - 1; if (n) counters.set(nk, n); else counters.delete(nk); continue; }
      queue.push([nx,ny,d]); if (next[3] < 2) counters.set(nk, 3);
    }
    events.push([x,y,incoming,flags]);
  }
  return { status: 0, events };
}

test('compiled Node/Web circuit exports and manifest describe the same ABI', async () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(BUILD, 'terra.manifest.json'), 'utf8'));
  assert.deepEqual(manifest.abi.circuit, { version: 1, sparseTopology: true, pauseBeforeExpansion: true });
  for (const target of ['node', 'web']) {
    const M = await moduleFor(target);
    assert.equal(M._terra_circuit_abi_version(), 1);
    for (const name of ['abi_version','create','close','stats','load','compile','patch','begin','step','cancel']) {
      assert.equal(typeof M[`_terra_circuit_${name}`], 'function');
      assert.ok(manifest.targets[target].exports.includes(`_terra_circuit_${name}`));
    }
  }
});

test('Wasm preserves source FIFO/counters/routes across 400 random colour passes, including bounded loops', async () => {
  const M = await moduleFor('node');
  let state = 0x184aba7;
  const random = n => { state ^= state << 13; state ^= state >>> 17; state ^= state << 5; return (state >>> 0) % n; };
  for (let trial = 0; trial < 100; trial++) {
    const rows = [], width = 8, height = 8;
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const mask = random(16), routing = random(6); if (mask) rows.push([x,y,mask,routing]);
    }
    const bridge = new Bridge(M, width, height, Math.max(1, rows.length));
    try {
      bridge.load(rows); bridge.compile(5);
      for (let colour = 0; colour < 4; colour++) {
        const seeds = [[0,0],[2,3],[2,3],[7,7]], expected = reference(rows,width,height,seeds,colour,512);
        assert.equal(bridge.begin(seeds,colour,true,512), 0);
        const events = []; let result;
        do { result = bridge.step(1,1); if (result.status >= 0) events.push(...result.events); } while (result.status === 1);
        assert.equal(result.status, expected.status); assert.deepEqual(events, expected.events);
      }
    } finally { bridge.close(); }
  }
});

test('tile hit pauses before its neighbors, patch affects the same pass, and cancellation can restart', async () => {
  const M = await moduleFor('node'), b = new Bridge(M,6,1,6);
  try {
    b.load(Array.from({length:6},(_,x)=>[x,0,1,x===2?1:0])); b.compile();
    assert.equal(b.begin([[0,0]],0,false),0);
    assert.deepEqual(b.step(),{status:1,processed:3,events:[[2,0,2,1]],remaining:1,totalProcessed:3});
    assert.equal(b.patch([[3,0,0,0]]),0);
    assert.deepEqual(b.step(),{status:0,processed:0,events:[],remaining:0,totalProcessed:3});
    assert.equal(b.patch([[3,0,1,1]]),0);
    assert.equal(b.begin([[0,0]],0,false),0); assert.equal(b.step().events[0][0],2);
    assert.equal(b.step().events[0][0],3);
    assert.equal(M._terra_circuit_cancel(b.handle),0);
    assert.equal(b.begin([[0,0]],0,false,2),0); assert.equal(b.step().status,-4);
    assert.equal(b.stats()[7],0);
    assert.equal(b.begin([[0,0]],0,false),0); assert.equal(b.step().events[0][0],2);
  } finally { b.close(); }
});

test('pixel axes record dangling in-bounds exits and exclude out-of-bounds exits', async () => {
  const M = await moduleFor('node');
  for (const height of [3,4]) {
    const b = new Bridge(M,3,height,1);
    try {
      b.load([[2,2,1,5]]); b.compile(); assert.equal(b.begin([[2,2]]),0);
      assert.deepEqual(b.step().events,[[2,2,0,height===3?3:11]]);
    } finally { b.close(); }
  }
});

test('actual Web Wasm handles one million cells and four million FIFO visits within Mini Program memory ceiling', async () => {
  const M = await moduleFor('web'), side = 1000, cells = side * side;
  const b = new Bridge(M,side,side,cells), buffer = new Uint32Array(65536 * 4);
  const start = performance.now();
  try {
    for (let first = 0; first < cells; first += 65536) {
      const count = Math.min(65536,cells-first);
      for(let i=0;i<count;i++){const k=first+i;buffer[i*4]=k%side;buffer[i*4+1]=Math.floor(k/side);buffer[i*4+2]=15;buffer[i*4+3]=k===cells-1?1:0;}
      M.HEAPU32.set(buffer.subarray(0,count*4),b.input/4);
      assert.equal(M._terra_circuit_load(b.handle,b.input,count),0);
    }
    b.compile(); const compiled = performance.now();
    for (let colour = 0; colour < 4; colour++) {
      assert.equal(b.begin([[0,0]],colour,false,cells+1),0);
      let visits = 0, outputs = 0, result;
      do { result = b.step(65536,1); assert.ok(result.status>=0); visits += result.processed; outputs += result.events.length;
        if(result.events.length)assert.deepEqual(result.events,[[999,999,2,1]]);
      } while(result.status===1);
      assert.equal(visits,cells);assert.equal(outputs,1);
    }
    const stats = b.stats();
    assert.ok(stats[11] < 48*1024*1024); assert.ok(stats[12] <= stats[13]);
    assert.ok(M.HEAPU8.byteLength <= 160*1024*1024);
    console.log(JSON.stringify({kind:'circuit-wasm-million',cells,visits:4000000,buildMs:+(compiled-start).toFixed(2),traversalMs:+(performance.now()-compiled).toFixed(2),activeBytes:stats[11],peakBytes:stats[12],linearMemoryBytes:M.HEAPU8.byteLength}));
  } finally { b.close(); }
});
