"use strict";

/* node tests/test_map_runtime_png.js <isolated-wld-node-js> <world.wld> <palette.tmrt> */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");

const [modulePath, worldPath, palettePath] = process.argv.slice(2);
if (!modulePath || !worldPath || !palettePath) {
  throw new Error("expected module, WLD fixture and validated TMRT fixture paths");
}

const create = require(path.resolve(modulePath));
const worldBytes = fs.readFileSync(worldPath);
const paletteBytes = fs.readFileSync(palettePath);

function alloc(m, size) {
  const ptr = m._tx_malloc(size);
  assert(ptr, `tx_malloc(${size}) failed`);
  return ptr;
}
function cstr(m, value) {
  const bytes = Buffer.from(`${value}\0`);
  const ptr = alloc(m, bytes.length);
  m.HEAPU8.set(bytes, ptr);
  return ptr;
}
function install(m, bytes) {
  const ptr = alloc(m, bytes.length);
  try {
    m.HEAPU8.set(bytes, ptr);
    assert.equal(m._txw_set_map_runtime(ptr, bytes.length), 0);
  } finally { m._tx_free(ptr); }
}
function render(m) {
  const input = alloc(m, worldBytes.length);
  const handlePtr = alloc(m, 4);
  let handle = 0;
  try {
    m.HEAPU8.set(worldBytes, input);
    assert.equal(m._terra_world_open_from_buffer(input, worldBytes.length, handlePtr), 0);
    handle = m.HEAPU32[handlePtr >>> 2];
    assert(handle);
    const name = cstr(m, "render_thumbnail_png");
    const request = cstr(m, '{"max_w":64}');
    const required = alloc(m, 8);
    let response = 0;
    try {
      assert.equal(m._terra_op_execute_json(handle, name, request, 0, 0n, required), 0);
      const size = m.HEAPU32[required >>> 2];
      response = alloc(m, size);
      assert.equal(m._terra_op_execute_json(handle, name, request, response, BigInt(size), required), 0);
    } finally {
      if (response) m._tx_free(response);
      m._tx_free(required); m._tx_free(request); m._tx_free(name);
    }
    const pngSize = alloc(m, 8), width = alloc(m, 4), height = alloc(m, 4);
    let output = 0;
    try {
      assert.equal(m._terra_op_get_thumbnail_png(handle, 0, 0n, pngSize, width, height), 2);
      const size = Number(new DataView(m.wasmMemory.buffer).getBigUint64(pngSize, true));
      assert(size > 40 && size < 8 * 1024 * 1024);
      output = alloc(m, size);
      assert.equal(m._terra_op_get_thumbnail_png(handle, output, BigInt(size), pngSize, width, height), 0);
      return Buffer.from(m.HEAPU8.slice(output, output + size));
    } finally {
      if (output) m._tx_free(output);
      m._tx_free(height); m._tx_free(width); m._tx_free(pngSize);
    }
  } finally {
    if (handle) m._terra_world_close(handle);
    m._tx_free(handlePtr); m._tx_free(input);
  }
}
function firstPixel(png) {
  assert.equal(png.subarray(0, 8).toString("hex"), "89504e470d0a1a0a");
  const idat = [];
  for (let offset = 8; offset < png.length;) {
    const length = png.readUInt32BE(offset);
    const type = png.toString("ascii", offset + 4, offset + 8);
    if (type === "IDAT") idat.push(png.subarray(offset + 8, offset + 8 + length));
    offset += length + 12;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  assert.equal(raw[0], 0, "first PNG scanline must use filter zero");
  return [...raw.subarray(1, 5)];
}

create().then((m) => {
  install(m, paletteBytes);
  const original = render(m);
  const modified = Buffer.from(paletteBytes);
  const tileCount = modified.readUInt32LE(20), wallCount = modified.readUInt32LE(24);
  const paletteCount = modified.readUInt32LE(28);
  const paletteOffset = 96 + (tileCount + wallCount) * 4;
  for (let i = 1; i < paletteCount; i++) {
    modified[paletteOffset + 4 * i] = 255;
    modified[paletteOffset + 4 * i + 1] = 0;
    modified[paletteOffset + 4 * i + 2] = 255;
  }
  install(m, modified);
  const changed = render(m);
  assert.notDeepEqual(firstPixel(changed), firstPixel(original));
  assert.notDeepEqual(changed, original);
  assert.equal(m._txw_set_map_runtime(0, 0), 0);
  console.log("TMRT changed actual PNG pixels:", firstPixel(original), "=>", firstPixel(changed));
}).catch((error) => { console.error(error); process.exitCode = 1; });
