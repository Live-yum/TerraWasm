/** RV32I program generator used only by circuit acceptance, never by the DUT. */
export function makeRv32iProgram() {
  const words = [], source = [], checks = [], labels = new Map(), reloc = []
  const emit = (word, asm) => { words.push(word >>> 0); source.push(asm); return (words.length - 1) * 4 }
  const mark = name => { if (labels.has(name)) throw new Error('duplicate label'); labels.set(name, words.length * 4) }
  const i = (op, rd, f3, rs1, imm) => (((imm & 4095) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op) >>> 0
  const r = (rd, f3, rs1, rs2, f7 = 0) => ((f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33) >>> 0
  const s = (rs2, rs1, imm, f3 = 2) => (((imm & 4064) << 20) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 31) << 7) | 0x23) >>> 0
  const b = (rs1, rs2, f3, off) => (((off & 4096) << 19) | ((off & 2016) << 20) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((off & 30) << 7) | ((off & 2048) >> 4) | 0x63) >>> 0
  const j = (rd, off) => (((off & 1048576) << 11) | ((off & 2046) << 20) | ((off & 2048) << 9) | (off & 1044480) | (rd << 7) | 0x6f) >>> 0
  const li = (rd, value) => {
    value |= 0
    if (value >= -2048 && value <= 2047) emit(i(0x13, rd, 0, 0, value), `addi x${rd}, x0, ${value}`)
    else {
      const hi = ((value + 0x800) >>> 12) & 0xfffff, lo = (value << 20) >> 20
      emit((hi << 12) | (rd << 7) | 0x37, `lui x${rd}, 0x${hi.toString(16)}`)
      if (lo) emit(i(0x13, rd, 0, rd, lo), `addi x${rd}, x${rd}, ${lo}`)
    }
  }
  const jump = (rd, label) => { reloc.push({ at: emit(0, `jal x${rd}, ${label}`) / 4, label, make: off => j(rd, off) }) }
  const branch = (rs1, rs2, f3, label, name) => { reloc.push({ at: emit(0, `${name} x${rs1}, x${rs2}, ${label}`) / 4, label, make: off => b(rs1, rs2, f3, off) }) }
  const save = (name, expected, rd = 3) => {
    const offset = checks.length * 4
    checks.push({ name, address: 0x100000 + offset, expected: expected >>> 0 })
    emit(s(rd, 31, offset), `sw x${rd}, ${offset}(x31) # ${name}`)
  }
  li(31, 0x100000)
  li(30, 0x100200)
  const imms = [
    ['addi', 0, 0x7fffffff, 1, 0x80000000],
    ['slti', 2, 0xffffffff, 1, 1],
    ['sltiu', 3, 0xffffffff, 1, 0],
    ['xori', 4, 0xa5a5a5a5, -1, 0x5a5a5a5a],
    ['ori', 6, 0x12345678, 0x345, 0x1234577d],
    ['andi', 7, 0xfedcba98, -2048, 0xfedcb800],
    ['slli', 1, 0x80000001, 31, 0x80000000],
    ['srli', 5, 0x80000001, 31, 1],
    ['srai', 5, 0x80000001, 0x41f, 0xffffffff],
  ]
  for (const [name, f3, value, imm, expected] of imms) {
    li(1, value); emit(i(0x13, 3, f3, 1, imm), `${name} x3, x1, ${imm & 31}`); save(name, expected)
  }
  const regs = [
    ['add', 0, 0, 0x80000000, 0x80000000, 0],
    ['sub', 0, 0x20, 0, 1, 0xffffffff],
    ['sll', 1, 0, 0x80000001, 33, 2],
    ['slt', 2, 0, 0xffffffff, 1, 1],
    ['sltu', 3, 0, 0xffffffff, 1, 0],
    ['xor', 4, 0, 0xa5a5a5a5, 0x3c3c3c3c, 0x99999999],
    ['srl', 5, 0, 0x80000001, 63, 1],
    ['sra', 5, 0x20, 0x80000001, 63, 0xffffffff],
    ['or', 6, 0, 0xa5a5a5a5, 0x3c3c3c3c, 0xbdbdbdbd],
    ['and', 7, 0, 0xa5a5a5a5, 0x3c3c3c3c, 0x24242424],
  ]
  for (const [name, f3, f7, a, bb, expected] of regs) {
    li(1, a); li(2, bb); emit(r(3, f3, 1, 2, f7), `${name} x3, x1, x2`); save(name, expected)
  }
  emit((0xabcde << 12) | (3 << 7) | 0x37, 'lui x3, 0xabcde'); save('lui', 0xabcde000)
  const auipcPc = words.length * 4
  emit((0x12 << 12) | (3 << 7) | 0x17, 'auipc x3, 0x12'); save('auipc', auipcPc + 0x12000)
  emit(i(0x13, 0, 0, 0, 99), 'addi x0, x0, 99'); save('x0 immutable', 0, 0)
  const jalPc = words.length * 4
  jump(3, 'jal_target'); li(3, 0xdead)
  mark('jal_target'); save('jal link and target', jalPc + 4)
  const jalrLoad = words.length
  emit(0, 'addi x1, x0, jalr_target+1')
  const jalrPc = emit(i(0x67, 3, 0, 1, 0), 'jalr x3, 0(x1)')
  li(3, 0xdead)
  mark('jalr_target'); words[jalrLoad] = i(0x13, 1, 0, 0, labels.get('jalr_target') + 1)
  save('jalr odd target and link', jalrPc + 4)
  const branches = [
    ['beq', 0, 7, 7, true], ['beq', 0, 7, 8, false],
    ['bne', 1, 7, 8, true], ['bne', 1, 7, 7, false],
    ['blt', 4, 0xffffffff, 1, true], ['blt', 4, 1, 0xffffffff, false],
    ['bge', 5, 1, 0xffffffff, true], ['bge', 5, 0xffffffff, 1, false],
    ['bltu', 6, 1, 0xffffffff, true], ['bltu', 6, 0xffffffff, 1, false],
    ['bgeu', 7, 0xffffffff, 1, true], ['bgeu', 7, 1, 0xffffffff, false],
  ]
  for (let k = 0; k < branches.length; k++) {
    const [name, f3, a, bb, expected] = branches[k]
    li(1, a); li(2, bb); li(3, 0)
    branch(1, 2, f3, `taken_${k}`, name)
    jump(0, `after_${k}`)
    mark(`taken_${k}`); li(3, 1)
    mark(`after_${k}`); save(`${name} ${expected ? 'taken' : 'not taken'}`, Number(expected))
  }
  li(1, 0x12345678); emit(s(1, 30, 0), 'sw x1, 0(x30)')
  emit(0x0ff0000f, 'fence iorw, iorw')
  emit(i(3, 3, 2, 30, 0), 'lw x3, 0(x30)'); save('fence store/load ordering', 0x12345678)
  li(1, 0x80ff7f01); emit(s(1, 30, 0), 'sw x1, 0(x30)')
  for (const [name, f3, offset, expected] of [
    ['lb positive', 0, 1, 0x7f], ['lb negative', 0, 3, 0xffffff80],
    ['lh positive', 1, 0, 0x7f01], ['lh negative', 1, 2, 0xffff80ff],
    ['lbu', 4, 2, 0xff], ['lhu', 5, 2, 0x80ff], ['lw', 2, 0, 0x80ff7f01],
  ]) { emit(i(3, 3, f3, 30, offset), `${name.split(' ')[0]} x3, ${offset}(x30)`); save(name, expected) }
  li(1, 0x11223344); emit(s(1, 30, 4), 'sw x1, 4(x30)')
  li(1, 0xab); emit(s(1, 30, 5, 0), 'sb x1, 5(x30)')
  emit(i(3, 3, 2, 30, 4), 'lw x3, 4(x30)'); save('sb lane preservation', 0x1122ab44)
  li(1, 0x9abc); emit(s(1, 30, 6, 1), 'sh x1, 6(x30)')
  emit(i(3, 3, 2, 30, 4), 'lw x3, 4(x30)'); save('sh lane preservation', 0x9abcab44)
  li(3, 55); li(4, 0); li(5, 10)
  mark('sum_loop')
  emit(r(4, 0, 4, 5), 'add x4, x4, x5')
  emit(i(0x13, 5, 0, 5, -1), 'addi x5, x5, -1')
  branch(5, 0, 1, 'sum_loop', 'bne')
  save('backwards branch sum', 55, 4)
  li(3, 0x600dc0de); save('completed', 0x600dc0de)
  mark('halt'); jump(0, 'halt')
  for (const { at, label, make } of reloc) {
    if (!labels.has(label)) throw new Error('undefined label ' + label)
    const off = labels.get(label) - at * 4
    words[at] = make(off)
  }
  const bytes = new Uint8Array(words.length * 4), dv = new DataView(bytes.buffer)
  words.forEach((word, idx) => dv.setUint32(idx * 4, word, true))
  return { bytes, words, checks, source, haltPc: labels.get('halt'), instructionKinds: [...new Set([...imms, ...regs, ...branches].map(row => row[0]).concat(['lui', 'auipc', 'jal', 'jalr', 'lb', 'lh', 'lw', 'lbu', 'lhu', 'sb', 'sh', 'sw', 'fence']))] }
}
