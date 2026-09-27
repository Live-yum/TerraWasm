'use strict';
const test=require('node:test'), assert=require('node:assert/strict'),fs=require('node:fs');
const {getPrimaryWorldPath}=require('./helpers/fixtures');
test('Web heap growth is admitted before allocation and capped without overgrowth',async()=>{
 const factory=require('../build/terrax_world_wasm_web.js');
 const wasmBinary=fs.readFileSync(require.resolve('../build/terrax_world_wasm_web.wasm'));
 let limit=70*1024*1024,calls=0;
 const M=await factory({wasmBinary,memoryGrowthLimit:()=>{calls++;return limit;}});
 assert.equal(M._tx_malloc(80*1024*1024),0);assert.ok(calls>0);assert.equal(M.HEAPU8.length,64*1024*1024);
 limit=90*1024*1024;const p=M._tx_malloc(80*1024*1024);assert.ok(p);assert.ok(M.HEAPU8.length<=limit);M._tx_free(p);
});
test('bounded source open, replay rejection, cancellation and repeated pixel adoption',async()=>{
 const M=await require('../build/terrax_world_wasm_web.js')({wasmBinary:fs.readFileSync(require.resolve('../build/terrax_world_wasm_web.wasm'))});
 const sources=new Map([[1,fs.readFileSync(getPrimaryWorldPath())]]),ptrs=[];
 const alloc=n=>{const p=M._tx_malloc(n);assert.ok(p);ptrs.push(p);return p;};
 const hp=alloc(4),ep=alloc(48),input=alloc(1048576),spec=alloc(40),maps=alloc(24);
 const check=n=>assert.equal(n,0);
 function pump(id,pixels){const pieces=[];let total=0;
  for(let steps=0;steps<1000000;steps++){
   check(M._terra_world_stream_step(id,64,ep));const e=Array.from(M.HEAPU32.subarray(ep/4,ep/4+12));assert.equal(e[0],1);
   if(e[1]===0)continue;
   check(M._terra_world_stream_step(id,1,ep));assert.deepEqual(Array.from(M.HEAPU32.subarray(ep/4,ep/4+12)),e);
   if(e[1]===1){assert.ok(e[4]<=1048576);const b=sources.get(e[2]).subarray(e[3],e[3]+e[4]);assert.equal(b.length,e[4]);M.HEAPU8.set(b,input);check(M._terra_world_stream_supply_source(id,e[2],e[3],input,e[4]));assert.equal(M._terra_world_stream_supply_source(id,e[2],e[3],input,e[4]),-1);}
   else if(e[1]===2){assert.ok(e[7]<=128);check(M._terra_world_stream_supply_pixels(id,0,0,0));}
   else if(e[1]===3){assert.ok(e[4]<=1048576);pieces.push([e[3],Buffer.from(M.HEAPU8.subarray(e[5],e[5]+e[4]))]);check(M._terra_world_stream_ack_output(id));}
   else if(e[1]===4){total=e[10];if(!pieces.length)return null;const b=Buffer.alloc(total);for(const [o,p]of pieces)p.copy(b,o);return b;}
   else assert.fail('unknown event');
  }assert.fail('task never finished');
 }
 const bad=Buffer.from(sources.get(1)),dimensions=Buffer.alloc(8);dimensions.writeInt32LE(2400);dimensions.writeInt32LE(8400,4);
 const dimensionAt=bad.indexOf(dimensions,bad.readUInt32LE(26));assert.ok(dimensionAt>=0);bad.writeInt32LE(1,dimensionAt);bad.writeInt32LE(1073741824,dimensionAt+4);sources.set(99,bad);
 check(M._terra_world_stream_open_begin(99,bad.length,hp));const invalidTask=M.HEAPU32[hp/4];assert.throws(()=>pump(invalidTask));check(M._terra_world_stream_close(invalidTask));
 check(M._terra_world_stream_open_begin(1,sources.get(1).length,hp));let id=M.HEAPU32[hp/4];pump(id);check(M._terra_world_stream_adopt(id,1,hp));let world=M.HEAPU32[hp/4];check(M._terra_world_stream_close(id));
 // Default nonzero index paints a tile without retaining any pixel records.
 M.HEAPU8.fill(0,maps,maps+24);M.HEAPU8[maps+12+3]=255;M.HEAPU8[maps+12+4]=1;M.HEAPU8[maps+12+10]=1;
 function begin(){M.HEAPU32.set([1,0,0,3,3,maps,2,1,0,0],spec/4);check(M._terra_world_stream_pixel_begin(world,spec,hp));return M.HEAPU32[hp/4];}
 id=begin();check(M._terra_world_stream_cancel(id));check(M._terra_world_stream_close(id));
 for(let n=2;n<=3;n++){id=begin();const b=pump(id);sources.set(n,b);check(M._terra_world_stream_adopt(id,n,hp));world=M.HEAPU32[hp/4];check(M._terra_world_stream_close(id));assert.ok(b.length>0);}
 check(M._terra_world_close(world));check(M._terra_world_stream_open_begin(3,sources.get(3).length,hp));id=M.HEAPU32[hp/4];pump(id);check(M._terra_world_stream_adopt(id,3,hp));check(M._terra_world_stream_close(id));check(M._terra_world_close(M.HEAPU32[hp/4]));
 assert.ok(M.HEAPU8.buffer.byteLength<=160*1024*1024);for(const p of ptrs)M._tx_free(p);
});
