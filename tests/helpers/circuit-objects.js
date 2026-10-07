'use strict';
// Independent public WorldFile/TileEntity encoders; no DUT or LFS input.
const { makeCircuitWorld } = require('./circuit-world');
const types = [21, 55, 85, 88, 378, 395, 423, 425, 467, 470, 471, 475, 520, 573, 597, 698, 723, 724];
const entityTiles = [378, 395, 423, 470, 471, 475, 520, 597, 698, 723, 724];
const entityShapes = [[2,3],[2,2],[1,1],[2,3],[3,3],[3,4],[1,1],[3,4],[1,2],[1,1],[1,1]];
const u16 = n => { const b = Buffer.alloc(2); b.writeUInt16LE(n & 65535); return b; };
const u32 = n => { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0); return b; };
const words = a => Buffer.concat(a.map(u32));
function string(s) { const b = Buffer.from(s), prefix = []; let n = b.length; while (n >= 128) { prefix.push(n & 127 | 128); n >>>= 7; } return Buffer.concat([Buffer.from([...prefix, n]), b]); }
const item = (id, prefix = 2, stack = 3) => Buffer.concat([u16(id), Buffer.from([prefix]), u16(stack)]);
function records() {
  return types.map((tile, i) => {
    const kind = entityTiles.indexOf(tile), section = kind >= 0 ? 5 : [21, 88, 467].includes(tile) ? 2 : 3;
    const [width, height] = kind >= 0 ? entityShapes[kind] : [tile === 88 ? 3 : 2, 2];
    let payload;
    if (section === 2) payload = Buffer.concat([string('箱名🧰\0'.repeat(180)), u32(200), ...Array.from({ length: 200 }, (_, slot) => slot % 17 ? u16(0) : Buffer.concat([u16(slot + 1), u32(500 + slot), Buffer.from([slot])]))]);
    else if (section === 3) payload = string(tile === 425 ? '完整中文🙂\0'.repeat(10000) : `文字${tile}`);
    else if ([1,4,6,8].includes(kind)) payload = item(100 + kind, kind + 1, 99);
    else if (kind === 0) payload = u16(37); // source NPC handle is not portable
    else if (kind === 2) payload = Buffer.from([1, 0]);
    else if (kind === 3) payload = Buffer.concat([Buffer.from([0x81, 0x42, 3, 7]), ...Array.from({ length: 7 }, (_, j) => item(200 + j, j, j + 1))]);
    else if (kind === 5) payload = Buffer.concat([Buffer.from([15]), ...Array.from({ length: 4 }, (_, j) => item(300 + j))]);
    else if (kind === 7) payload = Buffer.alloc(0);
    else payload = u16(kind === 9 ? 4367 : 2004);
    return { section, kind: Math.max(0, kind), tile, x: 4 + i * 5, y: 4, width, height, payload };
  });
}
function bundle(objects, originX = 3, originY = 4, version = 326) {
  const entries = objects.map(a => Buffer.concat([words([a.section, a.kind, a.x - originX, a.y - originY, a.tile, a.payload.length, 0, 0]), a.payload]));
  return Buffer.concat([words([0x31424f43, 1, version, objects.length, 32 + entries.reduce((n, b) => n + b.length, 0), originX, originY, 0]), ...entries]);
}
function sectionRecords(objects, section, dx = 0, dy = 0, firstId = 100, resetDummy = false, version = 326) {
  let id = firstId;
  const records = objects.filter(a => a.section === section).map(a => {
    let payload = resetDummy && section === 5 && a.kind === 0 ? u16(65535) : a.payload;
    if (section === 2 && version < 294) {
      let off = 0, size = 0, shift = 0, b;
      do { b = payload[off++]; size |= (b & 127) << shift; shift += 7; } while (b & 128);
      off += size; payload = Buffer.concat([payload.subarray(0, off), payload.subarray(off + 4)]);
    }
    if (section === 5 && a.kind === 3) {
      const bits = n => { let k = 0; for (; n; n >>>= 1) k += n & 1; return k; };
      const equip = bits(payload[0]) * 5, equip8 = payload[3] & 2 ? 5 : 0;
      const header = Buffer.from([payload[0], payload[1], ...(version >= 307 ? [payload[2]] : []), ...(version >= 308 ? [payload[3]] : [])]);
      payload = version === 311 ? Buffer.concat([header, payload.subarray(4, 4 + equip), payload.subarray(4 + equip + equip8), payload.subarray(4 + equip, 4 + equip + equip8)]) : Buffer.concat([header, payload.subarray(4)]);
    }
    if (section === 5 && version < 122) return Buffer.concat([u16(a.x + dx), u16(a.y + dy)]);
    const xy = section === 5 ? Buffer.concat([Buffer.from([a.kind]), u32(id++), u16(a.x + dx), u16(a.y + dy)]) : words([a.x + dx, a.y + dy]);
    return section === 3 ? Buffer.concat([payload, xy]) : Buffer.concat([xy, payload]);
  });
  return Buffer.concat([section === 5 ? u32(records.length) : u16(records.length), ...(section === 2 && version < 294 ? [u16(40)] : []), ...records]);
}
function replaceSections(b, replacements) {
  const table = b.readUInt32LE(0) >= 135 ? 26 : 6;
  const count = b.readUInt16LE(table - 2), starts = Array.from({ length: count }, (_, i) => b.readUInt32LE(table + i * 4)), format = Buffer.from(b.subarray(0, starts[0]));
  let offset = format.length;
  const parts = starts.map((start, i) => { const part = replacements[i] || b.subarray(start, starts[i + 1] || b.length); format.writeUInt32LE(offset, table + i * 4); offset += part.length; return part; });
  return Buffer.concat([format, ...parts]);
}
function fixture() {
  const objects = records(), cells = new Map(), geometry = [];
  for (let x = 3; x <= 92; x++) cells.set(`${x},4`, { x, y: 4, wires: 1 });
  for (const a of objects) for (let dx = 0; dx < a.width; dx++) for (let dy = 0; dy < a.height; dy++) {
    const x = a.x + dx, y = a.y + dy, key = `${x},${y}`;
    cells.set(key, { ...cells.get(key), x, y, type: a.tile, fx: dx * 18, fy: dy * 18 });
    geometry.push([a.tile, dx * 18 | dy * 18 << 16, dx | dy << 8 | a.width << 16 | a.height << 24, 0]);
  }
  const world = replaceSections(makeCircuitWorld([...cells.values()], 100, 32, 326), Object.fromEntries([2,3,5].map(section => [section, sectionRecords(objects, section)])));
  return { world, objects, geometry, companion: bundle(objects) };
}
function versionFixture(version) {
  const objects = [
    { section: 2, kind: 0, tile: 21, x: 4, y: 4, width: 2, height: 2, payload: Buffer.concat([string('神圣箱'), u32(40), u16(1999), u32(1225), Buffer.from([0]), Buffer.alloc(78)]) },
    { section: 3, kind: 0, tile: 55, x: 9, y: 4, width: 2, height: 2, payload: string('中文牌') },
  ];
  if (version >= 116) objects.push({ section: 5, kind: 0, tile: 378, x: 14, y: 4, width: 2, height: 3, payload: u16(version < 122 ? 65535 : 37) });
  if (version >= 122) objects.push({ section: 5, kind: 3, tile: 470, x: 19, y: 4, width: 2, height: 3,
    payload: Buffer.concat([Buffer.from([1, 1, version >= 307 ? 3 : 0, version >= 308 ? 7 : 0]), item(201), ...(version >= 308 ? [item(202)] : []), item(203), ...(version >= 308 ? [item(204), item(205)] : [])]) });
  const cells = new Map(), geometry = [];
  for (let x = 3; x <= 21; x++) cells.set(`${x},4`, { x, y: 4, wires: 1 });
  for (const a of objects) for (let dx = 0; dx < a.width; dx++) for (let dy = 0; dy < a.height; dy++) {
    const x = a.x + dx, y = a.y + dy;
    cells.set(`${x},${y}`, { ...cells.get(`${x},${y}`), x, y, type: a.tile, fx: dx * 18, fy: dy * 18 });
    geometry.push([a.tile, dx * 18 | dy * 18 << 16, dx | dy << 8 | a.width << 16 | a.height << 24, 0]);
  }
  const sections = [2, 3, ...(version >= 116 ? [5] : [])];
  const world = replaceSections(makeCircuitWorld([...cells.values()], 40, 32, version), Object.fromEntries(sections.map(section => [section, sectionRecords(objects, section, 0, 0, 100, false, version)])));
  return { world, objects, companion: bundle(objects, 3, 4, version), geometry,
    appended: Object.fromEntries(sections.map(section => [section, sectionRecords(objects, section, 0, 16, 102, true, version)])) };
}
module.exports = { fixture, versionFixture, bundle, records, sectionRecords, replaceSections, words };
