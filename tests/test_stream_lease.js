'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {fixture}=require('./generate-stream-fixtures');
test('compiled lease ABI rejects growth, replay, wrong owner/range and keeps legacy supply compatible',async()=>{
 const M=await require('../build/terrax_world_wasm.js')(),source=fixture(7,270,true),owned=[];
 const alloc=n=>{const p=M._tx_malloc(n);assert.ok(p);owned.push(p);return p;};
 const hp=alloc(4),ep=alloc(48),lp=alloc(32),sp=alloc(56),oldInput=alloc(1048576);
 const check=n=>assert.equal(n,0);assert.equal(M._terra_world_stream_abi_version(),2);
 check(M._terra_world_stream_open_begin(1,source.length,hp));const task=M.HEAPU32[hp/4];
 check(M._terra_world_stream_acquire_input(task,lp));const old=Array.from(M.HEAPU32.subarray(lp/4,lp/4+8)),before=M.HEAPU8.buffer;
 const growth=alloc(M.HEAPU8.length+65536);assert.notEqual(M.HEAPU8.buffer,before,'test must really grow Wasm memory');
 assert.equal(M._terra_world_stream_commit_input(task,old[1],old[2],old[3],old[4]),-1);
 assert.equal(M._terra_world_stream_release_input(task,old[1]),-1);
 let alternate=false;
 for(let steps=0;;steps++){
  assert.ok(steps<10000);check(M._terra_world_stream_step(task,1,ep));const e=Array.from(M.HEAPU32.subarray(ep/4,ep/4+12));
  if(e[1]===0)continue;if(e[1]===4)break;assert.equal(e[1],1);
  if(alternate){M.HEAPU8.set(source.subarray(e[3],e[3]+e[4]),oldInput);check(M._terra_world_stream_supply_source(task,1,e[3],oldInput,e[4]));}
  else{
   check(M._terra_world_stream_acquire_input(task,lp));const l=Array.from(M.HEAPU32.subarray(lp/4,lp/4+8));assert.equal(l[0],2);assert.equal(l[7],M.HEAPU8.length);
   assert.equal(M._terra_world_stream_commit_input(task+1,l[1],1,l[3],l[4]),-1);assert.equal(M._terra_world_stream_commit_input(task,l[1],1,l[3]+1,l[4]),-1);
   M.HEAPU8.set(source.subarray(l[3],l[3]+l[4]),l[5]);check(M._terra_world_stream_commit_input(task,l[1],1,l[3],l[4]));assert.equal(M._terra_world_stream_commit_input(task,l[1],1,l[3],l[4]),-1);
  }alternate=!alternate;
 }
 check(M._terra_world_stream_get_stats(task,sp));assert.ok(M.HEAPU32[sp/4+6]>0,'legacy supplies are counted');
 check(M._terra_world_stream_adopt(task,1,hp));const world=M.HEAPU32[hp/4];check(M._terra_world_stream_close(task));
 check(M._terra_world_stream_open_begin(2,source.length,hp));const cancelled=M.HEAPU32[hp/4];check(M._terra_world_stream_acquire_input(cancelled,lp));const lease=M.HEAPU32[lp/4+1];
 check(M._terra_world_stream_cancel(cancelled));assert.equal(M._terra_world_stream_commit_input(cancelled,lease,2,0,source.length),-1);check(M._terra_world_stream_close(cancelled));check(M._terra_world_close(world));
 for(const ptr of owned)M._tx_free(ptr);void growth;
});
