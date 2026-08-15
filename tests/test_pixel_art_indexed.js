"use strict";
const path = require("path");
const fs = require("fs");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const { getPrimaryWorldPath } = require("./helpers/fixtures");

const WLD_PATH = getPrimaryWorldPath();
const TXCI_PATH = path.resolve(__dirname, "..", "data", "terraria_color_index.txci");
const OUT_PATH = path.join(__dirname, "tmp_pixel_indexed.wld");

let passed = 0, failed = 0;
function ok(name) { passed++; console.log("  PASS: " + name); }
function fail(name, message) { failed++; console.log("  FAIL: " + name + " - " + message); }

function parseFormatPointers(buf) {
  let off = 0;
  const version = buf.readUInt32LE(off); off += 4;
  if (version >= 135) off += 20;
  const pointerCount = buf.readUInt16LE(off); off += 2;
  const positions = [];
  for (let i = 0; i < pointerCount; i++) {
    positions.push(buf.readUInt32LE(off));
    off += 4;
  }
  const tileTypeCount = buf.readUInt16LE(off); off += 2;
  const importantLen = Math.ceil(tileTypeCount / 8);
  const formatLen = off + importantLen;
  return { pointerCount, positions, formatLen };
}

async function main() {
  if (!fs.existsSync(WLD_PATH)) {
    console.log("SKIP: fixture world not found: " + WLD_PATH);
    return;
  }

  const M = await TerraWorldWasm();
  const { stringToUTF8, lengthBytesUTF8, getValue } = M;

  function as(str) {
    const len = lengthBytesUTF8(str) + 1;
    const ptr = M._tx_malloc(len);
    stringToUTF8(str, ptr, len);
    return ptr;
  }
  function errorJson() {
    const sizePtr = M._tx_malloc(8);
    M._terra_info_get_last_error_json(0, 0n, sizePtr);
    const size = M.HEAPU32[sizePtr >> 2];
    if (!size) return "";
    const ptr = M._tx_malloc(size);
    M._terra_info_get_last_error_json(ptr, BigInt(size), sizePtr);
    return M.UTF8ToString(ptr);
  }

  if (typeof M._txw_begin_pixel_art_indexed !== "function") {
    fail("indexed begin export", "missing _txw_begin_pixel_art_indexed");
    process.exit(1);
  }
  if (typeof M._txw_add_pixel_art_chunk !== "function") {
    fail("indexed chunk export", "missing _txw_add_pixel_art_chunk");
    process.exit(1);
  }
  ok("indexed exports exist");

  const hp = M._tx_malloc(4);
  let st = M._terra_world_open(as(WLD_PATH), hp);
  const h = getValue(hp, "i32");
  if (st !== 0 || !h) {
    fail("open world", "st=" + st + " " + errorJson());
    process.exit(1);
  }
  ok("open world");

  const txci = fs.readFileSync(TXCI_PATH);
  const txciPtr = M._tx_malloc(txci.length);
  M.HEAPU8.set(txci, txciPtr);

  const palette = new Uint8Array([
    0, 0, 0, 0,
    255, 0, 0, 255,
    0, 0, 255, 255,
  ]);
  const palettePtr = M._tx_malloc(palette.length);
  M.HEAPU8.set(palette, palettePtr);

  st = M._txw_begin_pixel_art_indexed(
    h,
    500, 500,
    16, 16,
    palettePtr, 3,
    txciPtr, txci.length,
    0, 0,
    0, 0,
    0
  );
  if (st !== 0) {
    fail("begin indexed pixel art", "st=" + st + " " + errorJson());
    process.exit(1);
  }
  ok("begin indexed pixel art");

  const indices = new Uint16Array(64 * 64);
  let used = 0;
  for (let y = 0; y < 16; y++) {
    for (let x = 0; x < 16; x++) {
      indices[y * 64 + x] = x < 8 ? 1 : 2;
      used++;
    }
  }
  const indexBytes = new Uint8Array(indices.buffer);
  const chunkPtr = M._tx_malloc(indexBytes.length);
  M.HEAPU8.set(indexBytes, chunkPtr);

  st = M._txw_add_pixel_art_chunk(h, 0, 0, chunkPtr, indices.length, used);
  if (st !== 0) {
    fail("add indexed chunk", "st=" + st + " " + errorJson());
    process.exit(1);
  }
  ok("add indexed chunk");

  st = M._terra_world_save(h, as(OUT_PATH));
  if (st !== 0) {
    fail("save indexed pixel art", "st=" + st + " " + errorJson());
    process.exit(1);
  }
  ok("save indexed pixel art");

  const out = fs.readFileSync(OUT_PATH);
  const fmt = parseFormatPointers(out);
  if (fmt.positions[0] === fmt.formatLen && fmt.positions.every((p, i, a) => i === 0 || p >= a[i - 1]) && fmt.positions[fmt.positions.length - 1] < out.length) {
    ok("format pointers recalculated");
  } else {
    fail("format pointers recalculated", JSON.stringify(fmt));
  }

  M._terra_world_close(h);

  const hp2 = M._tx_malloc(4);
  st = M._terra_world_open(as(OUT_PATH), hp2);
  const h2 = getValue(hp2, "i32");
  if (st === 0 && h2) {
    ok("reopen indexed output");
    M._terra_world_close(h2);
  } else {
    fail("reopen indexed output", "st=" + st + " " + errorJson());
  }

  if (fs.existsSync(OUT_PATH)) fs.unlinkSync(OUT_PATH);

  console.log("\n=== Results ===");
  console.log("  Passed: " + passed);
  console.log("  Failed: " + failed);
  process.exit(failed > 0 ? 1 : 0);
}

main().catch((err) => {
  console.error("FATAL:", err);
  process.exit(1);
});
