"use strict";

/* Hand encoded modern header fixtures. This intentionally does not call any
 * TerraWasm encoder: the byte order follows WorldFile.LoadWorldFlags. */
function makeSectionedWorld(version, options = {}) {
  if (![128, 129, 139, 195, 196, 326].includes(version)) throw new RangeError("header fixture version");
  const h = [];
  const u8 = n => h.push(n & 255);
  const i16 = n => { u8(n); u8(n >> 8); };
  const i32 = n => { for (let i = 0; i < 4; i++) u8(n >> (8 * i)); };
  const u32 = i32;
  const f32 = n => { const b = Buffer.alloc(4); b.writeFloatLE(n); h.push(...b); };
  const f64 = n => { const b = Buffer.alloc(8); b.writeDoubleLE(n); h.push(...b); };
  const bool = n => u8(n ? 1 : 0);
  const str = value => { const b = Buffer.from(String(value || ""), "utf8"); let n = b.length; while (n >= 128) { u8((n & 127) | 128); n >>>= 7; } u8(n); h.push(...b); };
  const zero = n => { for (let i = 0; i < n; i++) u8(0); };
  str(options.worldName || `header-v${version}`);
  if (version >= 179) { str("seed"); zero(8); }
  if (version >= 181) zero(16);
  i32(1); i32(0); i32(1000); i32(0); i32(500); i32(500); i32(1000);
  if (version >= 209) { i32(0); zero(9); } else if (version >= 112) bool(false);
  if (version >= 141) { const b = Buffer.alloc(8); b.writeBigInt64LE(11n); h.push(...b); }
  if (version >= 284) zero(8);
  u8(0); zero(3 * 4 + 4 * 4 + 3 * 4 + 4 * 4 + 3 * 4);
  i32(0); i32(0); f64(0); f64(0); f64(0); bool(true); i32(0); bool(false); bool(false);
  i32(0); i32(0); bool(false); zero(10); if (version >= 118) bool(false); zero(7);
  bool(false); bool(false); u8(0); i32(0); bool(false); if (version >= 257) bool(false);
  i32(0); i32(0); i32(0); f64(0);
  if (version >= 118) f64(0);
  if (version >= 113) u8(0);
  bool(false); u32(0); f32(0); i32(0); i32(0); i32(0); zero(8); i32(0); i16(0); f32(0);
  if (version >= 95) i32(0);
  if (version >= 99) bool(false);
  if (version >= 101) i32(0);
  if (version >= 104) bool(false);
  if (version >= 129) bool(options.savedTaxCollector);
  if (version >= 201) bool(false);
  if (version >= 107) i32(0);
  if (version >= 108) i32(0);
  if (version >= 109) i16(0);
  if (version >= 128) bool(options.fastForwardTimeToDawn);
  if (version >= 131) { bool(false); bool(false); bool(false); bool(false); bool(false); bool(false); bool(false); bool(false); bool(false); }
  if (version >= 140) zero(9);
  if (version >= 170) { zero(2); i32(0); i32(0); }
  if (version >= 174) { bool(false); i32(0); f32(0); f32(0); }
  if (version >= 178) zero(4);
  if (version > 194) u8(0);
  if (version >= 215) u8(0);
  if (version > 195) { u8(options.bgTree2 || 0); u8(options.bgTree3 || 0); u8(options.bgTree4 || 0); }
  if (version === 326) {
    // SaveWorldFlags at the pinned public game commit: book, lantern night,
    // treetops, holidays, ore tiers, pets/bosses/spawns, then modern seed tail.
    bool(false); i32(0); zero(3); i32(0); zero(2); zero(16); zero(3); zero(2);
    bool(false); bool(false); zero(8); zero(2); zero(7); zero(2); zero(2);
    zero(2); zero(8); zero(2); bool(false); zero(2); str('{}');
  }
  const metadata = version >= 135 ? Buffer.concat([Buffer.from("relogic"), Buffer.from([2]), Buffer.alloc(12)]) : Buffer.alloc(0);
  const pointerCount = version >= 220 ? 11 : version >= 189 ? 9 : version >= 170 ? 8 : 7;
  const formatLength = 4 + metadata.length + 2 + pointerCount * 4 + 2 + 1;
  const headerStart = formatLength;
  const fileLength = headerStart + h.length;
  const format = [Buffer.alloc(4), metadata, Buffer.alloc(2 + pointerCount * 4 + 3)];
  format[0].writeUInt32LE(version, 0);
  format[2].writeUInt16LE(pointerCount, 0);
  format[2].writeUInt32LE(headerStart, 2);
  for (let i = 1; i < pointerCount; i++) format[2].writeUInt32LE(fileLength, 2 + i * 4);
  format[2].writeUInt16LE(1, 2 + pointerCount * 4);
  return Buffer.concat([...format, Buffer.from(h)]);
}

module.exports = { makeSectionedWorld };
