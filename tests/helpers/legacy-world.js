"use strict";

/* Test fixture builder for the contiguous pre-release-88 WLD stream.  The
 * field order intentionally mirrors LoadWorld_Version1_Old_BeforeRelease88;
 * keeping it here makes every historical gate executable in compatibility
 * tests without committing a binary fixture. */
function makeLegacyWorld(version, options = {}) {
  if (!Number.isInteger(version) || version < 1 || version > 87) throw new RangeError("legacy WLD version");
  const b = [];
  const u8 = x => b.push(x & 255);
  const i16 = x => { u8(x); u8(x >> 8); };
  const i32 = x => { for (let i = 0; i < 4; ++i) u8(x >> (8 * i)); };
  const f32 = x => { const v = Buffer.alloc(4); v.writeFloatLE(x); b.push(...v); };
  const f64 = x => { const v = Buffer.alloc(8); v.writeDoubleLE(x); b.push(...v); };
  const bool = x => u8(x ? 1 : 0);
  const str = value => { const v = Buffer.from(String(value || ""), "utf8"); let n = v.length; while (n >= 128) { u8((n & 127) | 128); n >>>= 7; } u8(n); b.push(...v); };
  const zero = n => { for (let i = 0; i < n; ++i) u8(0); };
  const name = options.worldName || "Legacy World";
  i32(version); str(name);
  i32(options.worldId === undefined ? 87 : options.worldId);
  i32(0); i32(1); i32(0); i32(1); i32(1); i32(1);
  if (version >= 63) u8(2);
  if (version >= 44) zero(7 * 4);
  if (version >= 60) { zero(8 * 4); if (version >= 61) zero(2 * 4); }
  i32(0); i32(0); f64(0); f64(0); f64(0); bool(true); i32(0); bool(false);
  if (version >= 70) bool(false);
  i32(0); i32(0); if (version >= 56) bool(false);
  zero(3); if (version >= 66) bool(false); if (version >= 44) zero(4); if (version >= 64) zero(2);
  if (version >= 29) { bool(false); bool(false); if (version >= 34) { bool(false); if (version >= 80) bool(false); } bool(false); }
  if (version >= 32) bool(false); if (version >= 37) bool(false); if (version >= 56) bool(false);
  bool(false); bool(false); u8(0); if (version >= 23) { i32(0); bool(false); }
  i32(0); i32(0); i32(0); f64(0); if (version >= 113) u8(0);
  if (version >= 53) { bool(false); i32(0); f32(0); }
  if (version >= 54) { i32(107); i32(108); i32(111); }
  if (version >= 55) zero(3); if (version >= 60) { zero(5); i32(0); }
  if (version >= 62) { i16(0); f32(0); }
  // One important tile (type 3) with frame coordinates, then old RLE field.
  const tileType = options.tileType === undefined ? 3 : options.tileType;
  const skipsFrame = (version < 28 && tileType === 4) || (version < 40 && tileType === 19) || (version < 195 && tileType === 49);
  bool(true); if (version <= 77) u8(tileType); else i16(tileType);
  if (!skipsFrame) { i16(options.frameX ?? 12); i16(options.frameY ?? 34); }
  if (version >= 48) { bool(false); } // tile paint
  if (version <= 25) bool(false); // obsolete tile flag
  const wall = options.wall || 0;
  bool(wall !== 0); if (wall) { u8(wall); if (version >= 48) bool(false); }
  bool(false); // liquid
  if (version >= 33) bool(false); if (version >= 43) { bool(false); bool(false); }
  if (version >= 41) { bool(false); if (version >= 49) u8(0); }
  if (version >= 42) { bool(false); bool(false); }
  if (version >= 25) i16(0);
  // 1000 chest slots; one populated chest uses the historical item width.
  const chest = options.chest !== false; const slots = version < 58 ? 20 : 40;
  for (let c = 0; c < 1000; ++c) { const active = chest && c === 0; bool(active); if (!active) continue;
    i32(0); i32(0); if (version >= 85) str(options.chestName || "Chest");
    for (let i = 0; i < slots; ++i) { const item = i === 0; if (version < 59) u8(item ? 3 : 0); else i16(item ? 3 : 0);
      if (item) { if (version >= 38) i32(options.itemType || 1); else str(options.itemName || "Dirt"); if (version >= 36) u8(0); }
    }
  }
  const sign = options.sign !== false; for (let i = 0; i < 1000; ++i) { const active=sign && i===0; bool(active); if(active){str(options.signText || "Legacy sign"); i32(0); i32(0);} }
  bool(options.npc !== false); if (options.npc !== false) {
    if (version >= 190) i32(17); else str(options.npcTypeName || "Guide");
    if (version >= 83) str(options.npcName || "Guide");
    f32(1); f32(2); bool(false); i32(0); i32(0); bool(false);
  }
  // Historical town-name table (v31..83): 9 + 1 + (v65..83: 8) + v79 one.
  if (version >= 31 && version <= 83) { let count = 9; if (version >= 35) ++count; if (version >= 65) count += 8; if (version >= 79) ++count; for (let i=0;i<count;i++) str(i===4 ? "Old Guide" : ""); }
  if (version >= 7) { bool(true); str(name); i32(options.worldId === undefined ? 87 : options.worldId); }
  return Buffer.from(b);
}

module.exports = { makeLegacyWorld };
