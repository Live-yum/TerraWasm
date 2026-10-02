'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'files','1.wld'));
assert.equal(source.readInt32LE(0),326,'future test requires a genuine known-layout source');
const future=Buffer.from(source);future.writeInt32LE(328);
function allocations(M){const pointers=[];return{alloc(value){const bytes=typeof value==='number'?null:Buffer.from(value);const p=M._tx_malloc(bytes?bytes.length:value);assert(p);pointers.push(p);if(bytes)M.HEAPU8.set(bytes,p);return p;},free(){for(const p of pointers)M._tx_free(p);}};}
function text(M,p){let end=p;while(M.HEAPU8[end])end++;return Buffer.from(M.HEAPU8.subarray(p,end)).toString();}

test('future WLD read-only survives incremental open and all public mutation routes',async()=>{
 const M=await require('../build/terrax_world_wasm.js')();const a=allocations(M),input=a.alloc(future),hp=a.alloc(4),req=a.alloc(8),empty=a.alloc('{}\0');
 const task=M._terra_world_open_begin(input,future.length);assert(task);let status=10;
 for(let n=0;n<10000&&status===10;n++)status=M._terra_world_open_step(task,1);
 assert.equal(status,0);assert.equal(M._terra_world_open_finish(task,hp),0);const h=M.HEAPU32[hp/4];assert(h);M._terra_world_task_close(task);
 const fmt=a.alloc('format\0'),out=a.alloc(32768);assert.equal(M._terra_section_get_json(h,fmt,out,32768n,req),0);
 assert.deepEqual(((v)=>[v.version,v.originalVersion,v.readOnly,v.compatibility])(JSON.parse(text(M,out))),[328,328,true,'future-layout-readonly']);
 for(const op of ['header_patch','replace_chests','replace_bestiary','batch_update_tiles']){
   const name=a.alloc(op+'\0');assert.equal(M._terra_op_execute_json(h,name,empty,0,0n,req),4);
   assert.equal(M._terra_world_stream_operation_begin(h,name,empty,hp),-1);
 }
 assert.equal(M._terra_world_apply_commands(h,0,0),4);
 for(const name of ['_txw_queue_pixel_art','_txw_begin_pixel_art_indexed','_txw_add_pixel_art_chunk','_txw_add_pixel_art_chunks_bulk','_txw_add_pixel_art_chunks_bulk_fast','_txw_apply_pixel_art'])
   if(typeof M[name]==='function')assert.equal(M[name](h,...Array(16).fill(0)),-1,name);
 assert.equal(M._terra_world_stream_pixel_begin(h,0,hp),-1);
 assert.equal(M._terra_world_save_to_buffer(h,0,0,req),0);const len=M.HEAPU32[req/4];assert.equal(len,future.length);
 const saved=a.alloc(len);assert.equal(M._terra_world_save_to_buffer(h,saved,len,req),0);assert.deepEqual(Buffer.from(M.HEAPU8.subarray(saved,saved+len)),future);
 M._terra_world_close(h);
 // Old preview behavior allowed partial tiles. A future parse must reject it.
 const corrupt=Buffer.from(future),tile=corrupt.readUInt32LE(30);corrupt.set([128,255,127],tile);M.HEAPU8.set(corrupt,input);
 const bad=M._terra_world_open_begin(input,corrupt.length);assert(bad);status=10;
 for(let n=0;n<10000&&status===10;n++)status=M._terra_world_open_step(bad,1);
 assert.equal(status,5);M._terra_world_task_close(bad);a.free();
});

test('future stream source retains exact original bytes during save and adoption',async()=>{
 const M=await require('../build/terrax_world_wasm_web.js')({wasmBinary:fs.readFileSync(require.resolve('../build/terrax_world_wasm_web.wasm'))});
 const a=allocations(M),hp=a.alloc(4),ep=a.alloc(48),input=a.alloc(1048576),sources=new Map([[1,future]]);
 function pump(id){const pieces=[];for(let n=0;n<100000;n++){
   assert.equal(M._terra_world_stream_step(id,64,ep),0);const e=Array.from(M.HEAPU32.subarray(ep/4,ep/4+12));
   if(e[1]===0)continue;
   if(e[1]===1){const b=sources.get(e[2]).subarray(e[3],e[3]+e[4]);assert.equal(b.length,e[4]);M.HEAPU8.set(b,input);assert.equal(M._terra_world_stream_supply_source(id,e[2],e[3],input,e[4]),0);}
   else if(e[1]===3){pieces.push([e[3],Buffer.from(M.HEAPU8.subarray(e[5],e[5]+e[4]))]);assert.equal(M._terra_world_stream_ack_output(id),0);}
   else if(e[1]===4){if(!pieces.length)return null;const b=Buffer.alloc(e[10]);for(const [o,p]of pieces)p.copy(b,o);return b;}
   else assert.fail('unexpected event');
 }assert.fail('stream did not finish');}
 assert.equal(M._terra_world_stream_open_begin(1,future.length,hp),0);let id=M.HEAPU32[hp/4];pump(id);
 assert.equal(M._terra_world_stream_adopt(id,1,hp),0);let h=M.HEAPU32[hp/4];M._terra_world_stream_close(id);
 const name=a.alloc('save\0'),request=a.alloc('{}\0');
 for(let next=2;next<=3;next++){
   assert.equal(M._terra_world_stream_operation_begin(h,name,request,hp),0);id=M.HEAPU32[hp/4];const saved=pump(id);assert.deepEqual(saved,future);
   sources.set(next,saved);assert.equal(M._terra_world_stream_adopt(id,next,hp),0);h=M.HEAPU32[hp/4];M._terra_world_stream_close(id);
 }
 assert.equal(M._terra_world_stream_pixel_begin(h,0,hp),-1);
 M._terra_world_close(h);a.free();
});

test('future short-string metadata reports the actual synchronous task-step cost',async(t)=>{
 const M=await require('../build/terrax_world_wasm.js')();
 const start=source.readUInt32LE(26+8*4),end=source.readUInt32LE(26+9*4),count=15*1024*1024;
 const section=Buffer.alloc(count+12);section.writeInt32LE(count,4);
 const bytes=Buffer.concat([source.subarray(0,start),section,source.subarray(end)]);bytes.writeInt32LE(328);
 for(let i=9;i<11;i++)bytes.writeUInt32LE(source.readUInt32LE(26+i*4)-(end-start)+section.length,26+i*4);
 const a=allocations(M),input=a.alloc(bytes);const task=M._terra_world_open_begin(input,bytes.length);assert(task);
 let maximum=0,steps=0;
 while(M._terra_world_task_get_progress(task)<28){
   const begin=performance.now();assert.equal(M._terra_world_open_step(task,1),10);maximum=Math.max(maximum,performance.now()-begin);
   assert(++steps<1000);
 }
 t.diagnostic(`15 MiB empty-string metadata: largest measured step ${maximum.toFixed(3)} ms; not a viewer end-to-end benchmark`);
 assert.equal(M._terra_world_open_cancel(task),11);M._terra_world_task_close(task);a.free();
});
