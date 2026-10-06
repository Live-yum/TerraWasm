'use strict';
const test=require('node:test'), assert=require('node:assert/strict'),fs=require('node:fs');
const {getPrimaryWorldPath}=require('./helpers/fixtures');
test('streamed sources above 200 MB retain a bounded metadata image and signed WLD offsets',async()=>{
 const {fixture}=require('./generate-stream-fixtures');
 const base=fixture(123,321,true),count=base.readUInt16LE(24),tileStart=base.readUInt32LE(30),tileEnd=base.readUInt32LE(34);
 const prefix=Buffer.from(base.subarray(0,tileStart)),suffix=base.subarray(tileEnd),dimensions=Buffer.alloc(8);
 dimensions.writeInt32LE(321);dimensions.writeInt32LE(123,4);
 const position=prefix.indexOf(dimensions,base.readUInt32LE(26));assert.ok(position>=0);
 prefix.writeInt32LE(7200,position);prefix.writeInt32LE(15200,position+4);
 // 109,440,000 independent two-byte tile records. The virtual range source
 // represents a valid 218 MB world, but never allocates the tile section.
 const tileBytes=15200*7200*2,delta=tileBytes-(tileEnd-tileStart),size=prefix.length+tileBytes+suffix.length;
 for(let index=2;index<count;index++)prefix.writeUInt32LE(base.readUInt32LE(26+index*4)+delta,26+index*4);
 assert.ok(size>200000000&&size<0x80000000);
 function range(offset,length){
  const out=Buffer.alloc(length);assert.ok(length<=1048576&&offset+length<=size);
  for(let i=0;i<length;i++){
   const at=offset+i;
   out[i]=at<prefix.length?prefix[at]:at>=prefix.length+tileBytes?suffix[at-prefix.length-tileBytes]:(at-prefix.length)%2?1+(((at-prefix.length)>>1)&1):2;
  }
  return out;
 }
 const M=await require('../build/terrax_world_wasm_web.js')({wasmBinary:fs.readFileSync(require.resolve('../build/terrax_world_wasm_web.wasm'))});
 const hp=M._tx_malloc(4),ep=M._tx_malloc(48),input=M._tx_malloc(1048576);assert.ok(hp&&ep&&input);
 let task=0,world=0,maxRead=0,totalRead=0;
 try{
  assert.equal(M._terra_world_stream_open_begin(1,0x80000000,hp),-1,'WLD offsets remain signed Int32');
  assert.equal(M._terra_world_stream_open_begin(1,size,hp),0);task=M.HEAPU32[hp/4];
  for(;;){
   assert.equal(M._terra_world_stream_step(task,64,ep),0);
   const e=Array.from(M.HEAPU32.subarray(ep/4,ep/4+12));
   if(e[1]===4)break;
   if(e[1]===0)continue;
   assert.equal(e[1],1);assert.equal(e[2],1);
   maxRead=Math.max(maxRead,e[4]);totalRead+=e[4];M.HEAPU8.set(range(e[3],e[4]),input);
   assert.equal(M._terra_world_stream_supply_source(task,1,e[3],input,e[4]),0);
  }
  assert.equal(M._terra_world_stream_adopt(task,1,hp),0);world=M.HEAPU32[hp/4];
  assert.equal(M.HEAPU8.byteLength,64*1024*1024,'opening a large source does not allocate the file in Wasm');
  assert.ok(maxRead<=1048576,'each tile-index or metadata read stays within one MiB');
  assert.ok(totalRead>=tileBytes&&totalRead<=size+3*1048576,'the column index scans the source without retaining its tile bytes');
 }finally{
  if(task)assert.equal(M._terra_world_stream_close(task),0);
  if(world)assert.equal(M._terra_world_close(world),0);
  for(const pointer of [input,ep,hp])M._tx_free(pointer);
  assert.equal(M._tx_native_heap_used(),0);assert.equal(M._tx_bridge_heap_used(),0);
 }
});
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
