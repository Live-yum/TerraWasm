'use strict';
// Raw TXCI v3: one stone candidate; all 512 brick32 directories share group 0.
function minimalTxci() {
  const b = Buffer.alloc(4160);
  b.write('TXCI'); b.writeUInt16LE(3, 4); b.writeUInt16LE(32, 6);
  b.writeUInt32LE(1, 8); b.writeUInt32LE(1, 12); b.writeUInt32LE(512, 16);
  [44, 48, 56, 62, 4158].forEach((n, i) => b.writeUInt32LE(n, 20 + i * 4));
  b.set([128,128,128],44); b.writeUInt32LE(1,52); b.writeUInt16LE(1,56);
  return b;
}
module.exports = { minimalTxci };
