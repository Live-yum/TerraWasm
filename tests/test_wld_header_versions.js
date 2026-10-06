"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const { makeSectionedWorld } = require("./helpers/sectioned-world");

function alloc(M, bytes) { const p = M._tx_malloc(bytes.length); M.HEAPU8.set(bytes, p); return p; }
function op(M, handle, request) {
  const a = alloc(M, Buffer.from("header_patch\0")); const b = alloc(M, Buffer.from(`${JSON.stringify(request)}\0`)); const r = M._tx_malloc(8);
  try { assert.equal(M._terra_op_execute_json(handle, a, b, 0, 0n, r), 0); } finally { M._tx_free(a); M._tx_free(b); M._tx_free(r); }
}
function open(M, bytes) { const p = alloc(M, bytes); const out = M._tx_malloc(4); M.HEAPU8.set(bytes, p); const rc=M._terra_world_open_from_buffer(p, bytes.length, out); if(rc) { const e=M._tx_malloc(4096), n=M._tx_malloc(8); M._terra_info_get_last_error_json(e,4096n,n); const message=M.UTF8ToString(e); M._tx_free(e); M._tx_free(n); assert.fail(`v${bytes.readUInt32LE(0)} open: ${rc} ${message}`); } const h = M.HEAPU32[out >>> 2] >>> 0; M._tx_free(p); M._tx_free(out); return h; }
function header(M, handle) { const n = alloc(M, Buffer.from("header\0")); const r = M._tx_malloc(8); try { assert.equal(M._terra_section_get_json(handle, n, 0, 0n, r), 0); const size = M.HEAPU32[r >>> 2] >>> 0; const o = M._tx_malloc(size); try { assert.equal(M._terra_section_get_json(handle, n, o, BigInt(size), r), 0); return JSON.parse(M.UTF8ToString(o, size)); } finally { M._tx_free(o); } } finally { M._tx_free(n); M._tx_free(r); } }

function save(M, handle) {
  const required = M._tx_malloc(4); let output = 0;
  try {
    assert.equal(M._terra_world_save_to_buffer(handle, 0, 0, required), 0);
    const size = M.HEAPU32[required >>> 2]; output = M._tx_malloc(size);
    assert.equal(M._terra_world_save_to_buffer(handle, output, size, required), 0);
    return Buffer.from(M.HEAPU8.slice(output, output + size));
  } finally { if (output) M._tx_free(output); M._tx_free(required); }
}

test("header JSON preserves the complete long manifest from original, patched and reopened headers", async () => {
  const M = await TerraWorldWasm();
  const original = fs.readFileSync(path.join(__dirname, "files", "1.wld"));
  // Public LFS fixture: this independent span is pinned by native_contract.c.
  assert.equal(original[2453], 0xe3); assert.equal(original[2454], 0x4a);
  const expected = original.subarray(2455, original.readUInt32LE(30)).toString("utf8");
  assert.equal(Buffer.byteLength(expected), 9571);
  const parsed = JSON.parse(expected);
  let h = 0, reopened = 0;
  try {
    h = open(M, original);
    assert.equal(header(M, h).manifestJson, expected);
    assert.deepEqual(JSON.parse(header(M, h).manifestJson), parsed);
    op(M, h, { patch: { spawnTileX: 123 } });
    assert.equal(header(M, h).manifestJson, expected, "metadata changes must retain the entire active-header manifest");
    const saved = save(M, h);
    reopened = open(M, saved);
    assert.equal(header(M, reopened).manifestJson, expected);
    M._terra_world_close(reopened); reopened = 0;

    const longManifest = "\n\t" + JSON.stringify({
      padding: "电路实验😀".repeat(1800),
      escapes: "\"\\\b\f\t\n\r\0",
      finalField: "complete",
    }, null, 2) + "\r\n";
    assert.ok(Buffer.byteLength(longManifest) > 4095);
    op(M, h, { patch: { manifestJson: longManifest } });
    assert.equal(header(M, h).manifestJson, longManifest);
    reopened = open(M, save(M, h));
    const actual = header(M, reopened).manifestJson;
    assert.equal(actual, longManifest, "length-bound serialization must preserve UTF-8 and JSON escapes");
    assert.deepEqual(JSON.parse(actual), JSON.parse(longManifest));
  } finally { if (reopened) M._terra_world_close(reopened); if (h) M._terra_world_close(h); }
});

test("old xindong v326 exports omit lightning flags only before an empty manifest", async () => {
  const M = await TerraWorldWasm();
  const original = fs.readFileSync(path.join(__dirname, "files", "1.wld"));
  const tileStart = original.readUInt32LE(30);
  // Offsets independently pinned by native_contract.c's game fixture checks.
  assert.equal(original[2453], 0xe3);
  const prefix = Buffer.from(original.subarray(0, 2451));
  prefix.writeUInt32LE(326, 0);
  prefix.write("xindong", 4, "ascii");
  for (const tail of [[0], [], [0, 0], [0, 0, 0, 0]]) {
    const bytes = Buffer.concat([prefix, Buffer.from(tail), original.subarray(tileStart)]);
    const delta = tileStart - prefix.length - tail.length;
    for (let i = 1; i < bytes.readUInt16LE(24); i++) bytes.writeUInt32LE(original.readUInt32LE(26 + i * 4) - delta, 26 + i * 4);
    const input = alloc(M, bytes), out = M._tx_malloc(4);
    let h = 0, reopened = 0, output = 0;
    try {
      const rc = M._terra_world_open_from_buffer(input, bytes.length, out);
      h = M.HEAPU32[out >>> 2] >>> 0;
      if (tail.length !== 1) { assert.notEqual(rc, 0, `invalid ${tail.length}-byte header tail accepted`); continue; }
      assert.equal(rc, 0, "legacy xindong header rejected");
      assert.equal(header(M, h).moreLightningSeed, false);
      assert.equal(header(M, h).noLightningSeed, false);
      op(M, h, { patch: { spawnTileX: 123 } });
      assert.equal(M._terra_world_save_to_buffer(h, 0, 0, out), 0);
      const size = M.HEAPU32[out >>> 2];
      output = M._tx_malloc(size);
      assert.equal(M._terra_world_save_to_buffer(h, output, size, out), 0);
      const saved = Buffer.from(M.HEAPU8.slice(output, output + size));
      assert.deepEqual(saved.subarray(saved.readUInt32LE(30)), bytes.subarray(bytes.readUInt32LE(30)));
      M._terra_world_close(h); h = 0;
      reopened = open(M, saved);
      assert.equal(header(M, reopened).spawnTileX, 123);
      assert.equal(header(M, reopened).noLightningSeed, false);
    } finally {
      if (reopened) M._terra_world_close(reopened);
      if (h) M._terra_world_close(h);
      if (output) M._tx_free(output);
      M._tx_free(input); M._tx_free(out);
    }
  }
});

test("modern header gate boundaries stay aligned through parse and patch", async () => {
  const M = await TerraWorldWasm();
  for (const version of [128, 129, 139, 195, 196]) {
    const options = { savedTaxCollector: true, fastForwardTimeToDawn: true, bgTree2: 21, bgTree3: 22, bgTree4: 23 };
    const bytes = makeSectionedWorld(version, options);
    let h = 0; let reopened = 0;
    try {
      h = open(M, bytes);
      const before = header(M, h);
      if (version >= 129) assert.equal(before.savedTaxCollector, true);
      if (version >= 128) assert.equal(before.fastForwardTimeToDawn, true, `v${version} dawn`);
      if (version > 195) { assert.equal(before.treeBG2, 21); assert.equal(before.treeBG3, 22); assert.equal(before.treeBG4, 23); }
      const patch = { fastForwardTimeToDawn: false };
      if (version >= 129) patch.savedTaxCollector = false;
      op(M, h, { patch });
      if (version >= 129) assert.equal(header(M, h).savedTaxCollector, false);
      assert.equal(header(M, h).fastForwardTimeToDawn, false);
      const required = M._tx_malloc(4);
      let output = 0;
      try {
        assert.equal(M._terra_world_save_to_buffer(h, 0, 0, required), 0);
        const size = M.HEAPU32[required >>> 2];
        output = M._tx_malloc(size);
        assert.equal(M._terra_world_save_to_buffer(h, output, size, required), 0);
        const saved = Buffer.from(M.HEAPU8.slice(output, output + size));
        M._terra_world_close(h); h=0;
        reopened = open(M, saved);
        assert.equal(header(M, reopened).fastForwardTimeToDawn, false);
        if (version >= 129) assert.equal(header(M, reopened).savedTaxCollector, false);
        if (version > 195) assert.equal(header(M, reopened).treeBG4, 23);
      } finally { if (output) M._tx_free(output); M._tx_free(required); }

    } finally { if (reopened) M._terra_world_close(reopened); if (h) M._terra_world_close(h); }
  }
});
