/** Physical fixture mapping, calibrated against the actual published 279 world.
 * This module belongs to the test/loader harness, not the circuit evaluator.
 */
export const COMPUTERRARIA = Object.freeze({
  sourceCommit: '0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8',
  wireHeadCommit: 'e6009d010ca54ff43d04b44697accc7115807b9c',
  worldSha256: '55d0a24bd1f56d622003dbd30d52555e7d06d6d1bcacfc22ae506f2db5240c33',
  twldSha256: 'c6de694b3d034701513dc1ba17311213561ec359d3ecddde7bc35ea3c9611ed8',
  width: 15200, height: 7200, worldBytes: 405983441, formatVersion: 279,
  romBytes: 768 * 1024, ramBase: 0x100000, ramBytes: 368 * 1024,
  clock: { x: 3194, y: 153, mask: 8 },
  reset: { x: 3198, y: 156, mask: 4 },
  zeroDataBus: { x: 3243, y: 226, mask: 2 },
  zeroMemorySelect: { x: 3404, y: 350, mask: 2 },
  storePc: { x: 3198, y: 198, mask: 8 },
  readyLamp: { x: 3199, y: 156 },
})

function checkedWord(address, base, size) {
  if (!Number.isSafeInteger(address) || address < base || address >= base + size || address % 4) {
    throw new RangeError('word address is outside the pinned physical memory layout')
  }
  return (address - base) / 4
}

/** WireHead LoadCommand's current default: 2853+1g1ax8192g0x1,
 * 1143+1g3x32g131x24. Its old tinterface 96-KiB coordinates are not used. */
export function romBitCoordinate(address, bit) {
  const word = checkedWord(address, 0, COMPUTERRARIA.romBytes)
  if (!Number.isInteger(bit) || bit < 0 || bit > 31) throw new RangeError('bit index')
  const cell = word % 8192
  return { x: 2853 + cell + Math.floor((cell + 1) / 2), y: 1143 + Math.floor(word / 8192) * 131 + (31 - bit) * 3 }
}

/** Actual RAM uses two mirrored lamps per bit, 4096 words per bank, 23 banks;
 * its 125-row bank pitch differs from ROM. Both mirrors are patched on loading. */
export function ramBitCoordinate(address, bit, copy = 0) {
  const word = checkedWord(address, COMPUTERRARIA.ramBase, COMPUTERRARIA.ramBytes)
  if (!Number.isInteger(bit) || bit < 0 || bit > 31 || (copy !== 0 && copy !== 1)) throw new RangeError('bit or mirror index')
  return { x: 2853 + (word % 4096) * 3 + copy, y: 4287 + Math.floor(word / 4096) * 125 + (31 - bit) * 3 }
}

export function romLampWrites(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength % 4 || bytes.byteLength > COMPUTERRARIA.romBytes) throw new TypeError('aligned ROM image required')
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const records = []
  for (let address = 0; address < bytes.byteLength; address += 4) {
    const value = view.getUint32(address, true)
    for (let bit = 0; bit < 32; bit++) records.push({ ...romBitCoordinate(address, bit), value: (value >>> bit) & 1 })
  }
  return records
}

export function ramLampReads(addresses) {
  return addresses.flatMap(address => Array.from({ length: 32 }, (_, bit) => ({ ...ramBitCoordinate(address, bit), value: 0 })))
}

export function ramLampWrites(words) {
  return words.flatMap(({ address, value }) => Array.from({ length: 64 }, (_, position) => ({
    ...ramBitCoordinate(address, Math.floor(position / 2), position % 2), value: (value >>> Math.floor(position / 2)) & 1,
  })))
}

export function decodeRamWords(records, addresses) {
  if (records.length !== addresses.length * 32) throw new Error('RAM result count differs from request')
  return addresses.map((address, index) => {
    let value = 0
    for (let bit = 0; bit < 32; bit++) {
      const record = records[index * 32 + bit], coordinate = ramBitCoordinate(address, bit)
      if (record[0] !== coordinate.x || record[1] !== coordinate.y || record[3] !== 419 || record[2] > 1) throw new Error(`RAM lamp result mismatch at ${address.toString(16)} bit ${bit}`)
      value |= record[2] << bit
    }
    return value >>> 0
  })
}
