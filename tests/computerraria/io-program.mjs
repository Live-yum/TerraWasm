/** This ROM drives the physical memory-mapped display controller. The harness
 * never changes screen pixels itself: SW instructions execute in world gates. */
export function makeDisplayProgram(colorWord = 0xffffffff, monoWord = 0x80000001) {
  const words = [], source = []
  const emit = (word, asm) => { words.push(word >>> 0); source.push(asm) }
  const addi = (rd, rs, value) => emit(((value & 4095) << 20) | (rs << 15) | (rd << 7) | 0x13, `addi x${rd}, x${rs}, ${value}`)
  const li = (rd, value) => {
    value |= 0
    if (value >= -2048 && value <= 2047) addi(rd, 0, value)
    else {
      const high = ((value + 0x800) >>> 12) & 0xfffff, low = (value << 20) >> 20
      emit((high << 12) | (rd << 7) | 0x37, `lui x${rd}, 0x${high.toString(16)}`)
      if (low) addi(rd, rd, low)
    }
  }
  const sw = (rs, base, offset = 0) => emit(((offset & 4064) << 20) | (rs << 20) | (base << 15) | (2 << 12) | ((offset & 31) << 7) | 0x23, `sw x${rs}, ${offset}(x${base})`)
  li(31, 0x100000)
  li(1, 0x200000); li(2, colorWord); sw(2, 1)
  li(1, 0x203ffc); li(2, 1); sw(2, 1)
  li(1, 0x20e000); li(2, monoWord); sw(2, 1)
  li(2, 1); sw(2, 1, 508)
  li(3, 0x600dc0de); sw(3, 31, 188)
  emit(0x0000006f, 'jal x0, halt')
  const bytes = new Uint8Array(words.length * 4), view = new DataView(bytes.buffer)
  words.forEach((word, i) => view.setUint32(i * 4, word, true))
  return { bytes, words, source }
}
