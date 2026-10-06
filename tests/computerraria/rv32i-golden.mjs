/** Independent, test-only RV32I decoder. No import from the circuit runtime. */
export function runReference(program, maxInstructions = 10000) {
  const memory = new Uint8Array(0x210000)
  memory.set(program.bytes)
  const view = new DataView(memory.buffer), regs = new Int32Array(32)
  const stores = [], trace = []
  const sext = (n, bits) => (n << (32-bits)) >> (32-bits)
  let pc = 0, retired = 0
  const store = (addr, size, value) => {
    if (addr < 0x100000 || addr + size > 0x15c000) throw new Error(`unexpected reference store ${addr.toString(16)}`)
    if (size === 1) view.setUint8(addr, value)
    else if (size === 2) view.setUint16(addr, value, true)
    else view.setUint32(addr, value, true)
    stores.push({pc, address:addr, size, value: value >>> 0})
  }
  while (retired < maxInstructions) {
    if (pc === program.haltPc) break
    if (pc % 4 || pc < 0 || pc + 4 > program.bytes.length) throw new Error(`reference PC out of code: ${pc}`)
    const instruction = view.getUint32(pc, true)
    const opcode = instruction & 127, rd = (instruction >>> 7) & 31, funct3 = (instruction >>> 12) & 7
    const rs1 = (instruction >>> 15) & 31, rs2 = (instruction >>> 20) & 31, funct7 = instruction >>> 25
    const a = regs[rs1], b = regs[rs2], immediate = instruction >> 20
    let next = (pc + 4) | 0, result
    switch (opcode) {
      case 0x37: result = instruction & 0xfffff000; break
      case 0x17: result = pc + (instruction & 0xfffff000); break
      case 0x6f: {
        const off = sext(((instruction >>> 31) << 20) | (((instruction >>> 12)&255) << 12) | (((instruction >>> 20)&1) << 11) | (((instruction >>> 21)&1023) << 1),21)
        result = pc+4; next=pc+off; break
      }
      case 0x67: result=pc+4; next=(a+immediate)&~1; break
      case 0x63: {
        const off=sext(((instruction >>> 31)<<12)|(((instruction>>>7)&1)<<11)|(((instruction>>>25)&63)<<5)|(((instruction>>>8)&15)<<1),13)
        const cond = funct3===0 ? a===b : funct3===1 ? a!==b : funct3===4 ? a<b : funct3===5 ? a>=b : funct3===6 ? (a>>>0)<(b>>>0) : funct3===7 ? (a>>>0)>=(b>>>0) : undefined
        if(cond===undefined)throw new Error('bad reference branch')
        if(cond)next=pc+off
        break
      }
      case 0x13:
        switch(funct3){
          case 0: result=a+immediate;break
          case 2: result=Number(a<immediate);break
          case 3: result=Number((a>>>0)<(immediate>>>0));break
          case 4: result=a^immediate;break
          case 6: result=a|immediate;break
          case 7: result=a&immediate;break
          case 1: result=a<<(immediate&31);break
          case 5: result=funct7===32?a>>(immediate&31):a>>>(immediate&31);break
        }break
      case 0x33:
        switch(funct3){
          case 0: result=funct7===32?a-b:a+b;break
          case 1: result=a<<(b&31);break
          case 2: result=Number(a<b);break
          case 3: result=Number((a>>>0)<(b>>>0));break
          case 4: result=a^b;break
          case 5: result=funct7===32?a>>(b&31):a>>>(b&31);break
          case 6: result=a|b;break
          case 7: result=a&b;break
        }break
      case 0x03: {
        const addr=(a+immediate)>>>0
        if(funct3===0)result=view.getInt8(addr)
        else if(funct3===1)result=view.getInt16(addr,true)
        else if(funct3===2)result=view.getInt32(addr,true)
        else if(funct3===4)result=view.getUint8(addr)
        else if(funct3===5)result=view.getUint16(addr,true)
        else throw new Error('bad reference load')
        break
      }
      case 0x23: {
        const off=sext(((instruction>>>25)<<5)|((instruction>>>7)&31),12),addr=(a+off)>>>0
        if(funct3>2)throw new Error('bad reference store')
        store(addr,1<<funct3,b);break
      }
      case 0x0f:
        if (funct3 !== 0) throw new Error('reference fixture only supports FENCE, not FENCE.I')
        break // The reference is sequential and has no reordered memory effects.
      default: throw new Error(`reference unsupported instruction ${instruction.toString(16)} at ${pc}`)
    }
    if(result!==undefined&&rd)regs[rd]=result
    trace.push({pc,instruction,rd:result!==undefined?rd:null,value:result!==undefined?(regs[rd]>>>0):null})
    pc=next;retired++
  }
  if(pc!==program.haltPc)throw new Error(`reference instruction budget exhausted at ${pc}`)
  const signature=program.checks.map(check=>({ ...check, actual:view.getUint32(check.address,true) }))
  for(const check of signature)if(check.actual!==check.expected)throw new Error(`${check.name}: expected ${check.expected.toString(16)}, decoded ${check.actual.toString(16)}`)
  return {retired,signature,stores,trace,haltPc:pc}
}
