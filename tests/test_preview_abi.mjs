import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
const require = createRequire(import.meta.url)
const root = path.resolve(import.meta.dirname, '..'), profile = process.argv[2] || 'plr'
const directory = path.resolve(process.argv[3] || path.join(root, `build-preview-${profile}`))
const target = process.argv[4] || 'node'
assert(['node', 'web'].includes(target))
const moduleName = target === 'web' ? 'terrax_world_wasm_web' : 'terrax_world_wasm'
const M = await require(path.join(directory, `${moduleName}.js`))({ wasmBinary: fs.readFileSync(path.join(directory, `${moduleName}.wasm`)) })
const alloc = size => { const p = M._tx_malloc(size); assert(p); return p }
const baseline = M._tx_native_heap_used()
if (profile === 'plr') {
  const corpus = process.env.TERRAWASM_PREVIEW_CORPUS || path.join(root, 'tests/files')
  const walk = dir => fs.readdirSync(dir, { withFileTypes: true }).flatMap(e => e.isDirectory() ? walk(path.join(dir,e.name)) : /\.plr$/i.test(e.name) ? [path.join(dir,e.name)] : [])
  let count = 0
  for (const file of walk(corpus)) {
    const bytes = fs.readFileSync(file), input = alloc(bytes.length), size = alloc(4)
    let output = 0
    try {
      M.HEAPU8.set(bytes,input)
      assert.equal(M._terra_plr_preview_json_from_buffer(input,bytes.length,0,0n,size),0,file)
      const length = M.HEAPU32[size>>>2]; output = alloc(length)
      assert.equal(M._terra_plr_preview_json_from_buffer(input,bytes.length,output,BigInt(length),size),0,file)
      const model = JSON.parse(new TextDecoder().decode(M.HEAPU8.subarray(output,output+length-1)))
      assert(model.name && model.armor.length === 20 && model.inventory.length === 58)
      assert.notEqual(M._terra_plr_preview_json_from_buffer(input,16,0,0n,size),0)
      count++
    } finally { if(output)M._tx_free(output); M._tx_free(size); M._tx_free(input) }
    assert.equal(M._tx_native_heap_used(),baseline,'preview retains no native model')
  }
  assert(count > 0, 'preview fixture corpus must not be empty')
  console.log(`PLR preview WASM: ${count} real files, truncated inputs rejected, no retained native allocations`)
} else {
  const { makeCircuitWorld } = require('./helpers/circuit-world.js')
  // Independent binary records: every known flag, high wall byte, shimmer,
  // negative saved frame, timer enabled frame, and RLE crossing the requested rect.
  const first = [111,95,255,30,58,1,18,0,255,255,17,2,8,193,1,2]
  const timer = [66,144,0,0,18,0,2]
  for (const version of [269,314,319,326]) {
    const base = makeCircuitWorld([],2,3,version), start=base.readUInt32LE(30), end=base.readUInt32LE(34)
    const tiles = Buffer.from([...first,...timer])
    const bytes = Buffer.concat([base.subarray(0,start),tiles,base.subarray(end)])
    for(let i=2;i<bytes.readUInt16LE(24);i++) bytes.writeUInt32LE(base.readUInt32LE(26+i*4)+tiles.length-(end-start),26+i*4)
    const input=alloc(bytes.length),columns=alloc(12),output=alloc(2*3*8*4)
    try {
      M.HEAPU8.set(bytes,input)
      assert.equal(M._terra_world_preview_index(input,bytes.length,2,3,columns,3),0)
      assert.equal(M._terra_world_preview_read_rect(input,bytes.length,2,3,columns,0,0,2,3,output,48),0)
      const expected=[]
      for(let x=0;x<2;x++) for(let y=0;y<3;y++) expected.push(x,y,
        x?144|65536:314|(127<<16),x?18<<16:0xffff0012,x?0:258|(17<<16)|(8<<24),x?0:193|(4<<8)|(5<<16)|(15<<24),0,0)
      assert.deepEqual(M.HEAPU32.slice(output>>>2,(output>>>2)+48),Uint32Array.from(expected))
      assert.equal(M._terra_world_preview_read_rect(input,bytes.length,2,3,columns,1,1,1,1,output,8),0)
      assert.deepEqual(M.HEAPU32.slice(output>>>2,(output>>>2)+8),Uint32Array.of(0,0,144|65536,18<<16,0,0,0,0))
      assert.notEqual(M._terra_world_preview_read_rect(input,bytes.length,2,3,columns,2,0,1,1,output,8),0)
      assert.notEqual(M._terra_world_preview_index(input,64,2,3,columns,3),0)
      M.HEAPU8[input+start+15]=3
      assert.notEqual(M._terra_world_preview_index(input,bytes.length,2,3,columns,3),0,'RLE cannot cross a column')
      M.HEAPU8[input+start+15]=2;M.HEAPU8[input+start+3]|=1
      assert.notEqual(M._terra_world_preview_index(input,bytes.length,2,3,columns,3),0,'unknown extension flags fail closed')
    } finally { M._tx_free(output);M._tx_free(columns);M._tx_free(input) }
    assert.equal(M._tx_native_heap_used(),baseline)
  }
  console.log('WLD preview WASM: versions 269/314/319/326, exact Tile32, raw timer frames, all flags, RLE, invalid bounds and truncated/unknown data, no retained allocations')
}
assert.equal(M._tx_bridge_heap_used(),0)
