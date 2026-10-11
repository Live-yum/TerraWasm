'use strict';
// Independent BinaryWriter fixtures: no TerraWasm encoder generates a golden.
const fs = require('node:fs');
const path = require('node:path');
const { makeSectionedWorld } = require('./helpers/sectioned-world');
const { makeCircuitWorld } = require('./helpers/circuit-world');

const original = {
  worldName: 'identical-prefix-'.repeat(40) + '世界"\\\n尾',
  seed: 'error|invisible plane|'.repeat(27) + '"\\\n\t\0尾',
  anglerWhoFinishedToday: ['angler'.repeat(130) + '钓鱼者', 'second"\\\n\0tail'],
  manifestJson: JSON.stringify({ unknown: 'manifest"\\\n'.repeat(800), tail: '完整' }),
};

function strings(version, values) {
  return {
    worldName: values.worldName,
    seed: version < 179 ? '' : version === 179 ? String(values.seed ?? 123) : values.seed,
    anglerWhoFinishedToday: version >= 95 ? values.anglerWhoFinishedToday : [],
    manifestJson: version >= 299 ? values.manifestJson : '',
  };
}

function fixture(version, values) {
  const base = makeCircuitWorld([], 7, 11, version);
  const table = version >= 135 ? 26 : 6, count = base.readUInt16LE(table - 2);
  const pointers = Array.from({ length: count }, (_, i) => base.readUInt32LE(table + i * 4));
  const format = Buffer.from(base.subarray(0, pointers[0]));
  const sections = pointers.map((start, i) => Buffer.from(base.subarray(start, pointers[i + 1] || base.length)));
  const headerFile = makeSectionedWorld(version, values);
  const header = Buffer.from(headerFile.subarray(headerFile.readUInt32LE(table)));
  // Dimensions follow the identity fields; these independent constants occur
  // nowhere in the fixture's long strings or other metadata.
  const dimensions = Buffer.from([244, 1, 0, 0, 232, 3, 0, 0]);
  const at = header.indexOf(dimensions);
  if (at < 0) throw new Error('fixture dimensions missing');
  header.writeInt32LE(11, at); header.writeInt32LE(7, at + 4);
  sections[0] = header;
  const name = Buffer.from(values.worldName), prefix = [];
  let n = name.length;
  while (n >= 128) { prefix.push((n & 127) | 128); n >>>= 7; }
  if (values.paddedStringPrefixes) prefix.push(n | 128, 0); else prefix.push(n);
  const id = Buffer.alloc(4); id.writeInt32LE(values.worldId ?? 1);
  sections[count - 1] = Buffer.concat([Buffer.from([1, ...prefix]), name, id]);
  let offset = format.length;
  sections.forEach((section, i) => { format.writeUInt32LE(offset, table + i * 4); offset += section.length; });
  return Buffer.concat([format, ...sections]);
}

function generate(dir) {
  fs.mkdirSync(dir, { recursive: true });
  for (const version of [88, 179, 293, 326]) {
    const initial = { ...original, ...(version === 179 ? { seed: '123' } : {}) };
    const moved = { ...initial, spawnTileX: 1 };
    const twice = { ...moved, spawnTileY: 2 };
    const identity = { ...twice, worldId: 2 };
    // The first 159 bytes are identical. A summary-only comparison misses the
    // changed suffix and leaves Terraria's physical footer inconsistent.
    const patch = { worldName: original.worldName + '-changed' };
    if (version >= 179) patch.seed = version === 179 ? '4294967295' : 'new-种子"\\\n'.repeat(1200);
    if (version >= 299) patch.manifestJson = JSON.stringify({ changed: 'new-manifest'.repeat(1000) });
    const changed = { ...identity, ...patch };
    for (const [label, values] of Object.entries({ original: initial, moved, twice, identity, changed })) {
      fs.writeFileSync(path.join(dir, `v${version}-${label}.wld`), fixture(version, values));
      fs.writeFileSync(path.join(dir, `v${version}-${label}.json`), JSON.stringify(strings(version, values)));
    }
    fs.writeFileSync(path.join(dir, `v${version}-patch.json`), JSON.stringify({ patch }));
  }
  const padded = { ...original, paddedStringPrefixes: true };
  fs.writeFileSync(path.join(dir, 'padded-original.wld'), fixture(326, padded));
  fs.writeFileSync(path.join(dir, 'padded-moved.wld'), fixture(326, { ...padded, spawnTileX: 1 }));
  fs.writeFileSync(path.join(dir, 'padded-original.json'), JSON.stringify(strings(326, padded)));
}

if (require.main === module) generate(process.argv[2]);
module.exports = { generate, fixture, original };
