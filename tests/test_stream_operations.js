'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),zlib=require('node:zlib');
const {getPrimaryWorldPath}=require('./helpers/fixtures');
function fixture(){const b=fs.readFileSync(getPrimaryWorldPath()),n=b.readUInt16LE(24),starts=Array.from({length:n},(_,i)=>b.readUInt32LE(26+i*4));
 const parts=starts.map((s,i)=>Buffer.from(b.subarray(s,starts[i+1]||b.length))),pair=Buffer.alloc(8);pair.writeInt32LE(2400);pair.writeInt32LE(8400,4);
 const at=parts[0].indexOf(pair);assert.ok(at>=0);
 const expanded=Buffer.alloc(2400*4);for(let i=0;i<2400;i++)expanded.set([130,1,0,0],i*4);
 parts[1]=Buffer.concat(Array.from({length:8400},(_,x)=>x<128?expanded:Buffer.from([130,1,95,9])));
 const format=Buffer.from(b.subarray(0,starts[0]));let off=format.length;parts.forEach((p,i)=>{format.writeUInt32LE(off,26+i*4);off+=p.length;});return Buffer.concat([format,...parts]);}
function pngPixels(b){let at=8,raw=[];while(at<b.length){const n=b.readUInt32BE(at),tag=b.toString('ascii',at+4,at+8);if(tag==='IDAT')raw.push(b.subarray(at+8,at+8+n));at+=12+n;}return zlib.inflateSync(Buffer.concat(raw));}
test('source-backed thumbnail, full PNG, MAP and ordinary mutations match memory-backed operations',async t=>{
 let growthLimit=process.env.STREAM_CACHE_LIMIT?Number(process.env.STREAM_CACHE_LIMIT):167772160;
 const M=process.env.STREAM_WEB?await require('../build/terrax_world_wasm_web.js')({wasmBinary:fs.readFileSync(require.resolve('../build/terrax_world_wasm_web.wasm')),memoryGrowthLimit:()=>growthLimit}):await require('../build/terrax_world_wasm.js')(),sources=new Map([[1,fixture()]]),ptrs=[];
 const alloc=b=>{const p=M._tx_malloc(typeof b==='number'?b:b.length);assert.ok(p);ptrs.push(p);if(typeof b!=='number')M.HEAPU8.set(b,p);return p;},str=s=>alloc(Buffer.from(s+'\0'));
 const hp=alloc(4),ep=alloc(48),input=alloc(1048576),size=alloc(8);
 const check=n=>{if(n){M._terra_info_get_last_error_json(0,0n,size);const p=alloc(M.HEAPU32[size/4]);M._terra_info_get_last_error_json(p,BigInt(M.HEAPU32[size/4]),size);assert.equal(n,0,Buffer.from(M.HEAPU8.subarray(p,M.HEAPU8.indexOf(0,p))).toString());}};
 function pump(id,onSource,onStep,units=10,stopAtHeader=false){const pieces=[],sourceOffsets=[];let sourceBytes=0,outputEvents=0,headerStep=-1;for(let steps=0;steps<100000;steps++){
  onStep?.(steps,sourceOffsets);
  check(M._terra_world_stream_step(id,units,ep));const e=Array.from(M.HEAPU32.subarray(ep/4,ep/4+12));
  if(!e[1])continue;if(e[1]===1){sourceBytes+=e[4];sourceOffsets.push(e[3]);onSource?.(e[3],sourceOffsets);const b=sources.get(e[2]).subarray(e[3],e[3]+e[4]);assert.equal(b.length,e[4]);M.HEAPU8.set(b,input);check(M._terra_world_stream_supply_source(id,e[2],e[3],input,e[4]));}
  else if(e[1]===3){assert.ok(e[4]<=1048576);if(e[3]===0&&headerStep<0)headerStep=steps;if(stopAtHeader&&e[3]===0)return {headerStep};outputEvents++;pieces.push([e[3],Buffer.from(M.HEAPU8.subarray(e[5],e[5]+e[4]))]);check(M._terra_world_stream_ack_output(id));}
  else if(e[1]===4){const bytes=Buffer.alloc(e[10]);pieces.forEach(([off,b])=>b.copy(bytes,off));return {bytes,kind:e[11],sourceBytes,sourceOffsets,outputEvents,headerStep};}else assert.fail('unexpected event');
 }assert.fail('operation did not finish');}
 function oldOpen(){const b=sources.get(1),p=alloc(b);if(M._terra_world_open_from_buffer)check(M._terra_world_open_from_buffer(p,b.length,hp));else{const task=M._terra_world_open_begin(p,b.length);assert.ok(task);let status=10;while(status===10)status=M._terra_world_open_step(task,100);check(status);check(M._terra_world_open_finish(task,hp));M._terra_world_task_close(task);}return M.HEAPU32[hp/4];}
 function oldOp(w,name,request){const np=str(name),rp=str(JSON.stringify(request));check(M._terra_op_execute_json(w,np,rp,0,0n,size));}
 function oldMedia(w,map){assert.equal(M[map?'_terra_op_get_map':'_terra_op_get_thumbnail_png'](w,0,0n,size,0,0),2);const n=M.HEAPU32[size/4],p=alloc(n);check(M[map?'_terra_op_get_map':'_terra_op_get_thumbnail_png'](w,p,BigInt(n),size,0,0));return Buffer.from(M.HEAPU8.subarray(p,p+n));}
 function streamed(name,request,onSource,onStep,units){check(M._terra_world_stream_operation_begin(world,str(name),str(JSON.stringify(request)),hp));const id=M.HEAPU32[hp/4],result=pump(id,onSource,onStep,units);return {id,...result};}
 let w=oldOpen();oldOp(w,'render_thumbnail_png',{max_w:64});const thumb=oldMedia(w,false);oldOp(w,'render_preview_png',{});const full=oldMedia(w,false);oldOp(w,'render_lit_map',{});const map=oldMedia(w,true);check(M._terra_world_close(w));
 check(M._terra_world_stream_open_begin(1,sources.get(1).length,hp));let id=M.HEAPU32[hp/4];pump(id);check(M._terra_world_stream_adopt(id,1,hp));let world=M.HEAPU32[hp/4];check(M._terra_world_stream_close(id));
 for(const [name,request,expected,kind]of [['render_thumbnail_png',{max_w:64},thumb,1],['render_preview_png',{},full,1],['render_lit_map',{},map,2]]){
  const r=streamed(name,request);assert.equal(r.kind,kind);if(kind===1)assert.deepEqual(pngPixels(r.bytes),pngPixels(expected));else assert.equal(Buffer.compare(r.bytes,expected),0);check(M._terra_world_stream_close(r.id));
  if(name==='render_lit_map'){
   const rewinds=r.sourceOffsets.filter((offset,i)=>i&&offset<r.sourceOffsets[i-1]).length;
   assert.equal(rewinds,0,'plain indexed MAP should read columns in order once');
   assert.ok(r.sourceBytes<sources.get(1).length*2,`MAP reread ${r.sourceBytes} of ${sources.get(1).length} source bytes`);
   assert.ok(r.outputEvents<20,`MAP emitted ${r.outputEvents} fragments`);
   t.diagnostic(`MAP source bytes: ${r.sourceBytes}; output events: ${r.outputEvents}; map size: ${r.bytes.length}`);
  }
  if(name==='render_preview_png'&&process.env.STREAM_WEB){if(process.env.STREAM_CACHE_LIMIT){assert.ok(r.sourceBytes>1048576);assert.ok(M.HEAPU8.length<=Number(process.env.STREAM_CACHE_LIMIT));}else assert.ok(r.sourceBytes<=sources.get(1).length*3);}
 }
 const marked=streamed('render_lit_map',{tile_markers:[{tile_type:2,locate:1},{tile_type:2,locate:2}]});
 assert.equal(Buffer.compare(marked.bytes,map),0,'unmatched markers must preserve MAP bytes');
 assert.equal(marked.sourceOffsets.filter((offset,i)=>i&&offset<marked.sourceOffsets[i-1]).length,1,'tile markers retain the scan pass');
 check(M._terra_world_stream_close(marked.id));
 const source=sources.get(1),tileStart=source.readUInt32LE(30),saved=source[tileStart+3];
 source[tileStart+3]=1;
 try{
  check(M._terra_world_stream_operation_begin(world,str('render_lit_map'),str('{}'),hp));
  const badId=M.HEAPU32[hp/4];
  assert.throws(()=>pump(badId),/indexed map column length mismatch/,'changed source must fail its saved column index');
  check(M._terra_world_stream_close(badId));
 }finally{source[tileStart+3]=saved;}
 if(process.env.STREAM_WEB&&!process.env.STREAM_CACHE_LIMIT){
  const held=[];
  try{
   const nativeBytes=[];
   const probe=streamed('render_lit_map',{},null,step=>nativeBytes[step]=M._tx_native_heap_used(),1);
   check(M._terra_world_stream_close(probe.id));
   assert.ok(probe.headerStep>2);
   assert.equal(nativeBytes[probe.headerStep-2],nativeBytes[probe.headerStep-1],'MAP staging must be complete before memory pressure');
   const fallback=streamed('render_lit_map',{},null,step=>{
    if(held.length||step!==probe.headerStep-1)return;
    assert.equal(M._tx_native_heap_used(),nativeBytes[step],'MAP staging changed before output allocation');
    growthLimit=M.HEAPU8.length;
    for(let p;(p=M._tx_malloc(65536));)held.push(p);
    assert.ok(held.length>10,'could not constrain WASM heap for MAP fallback');
    for(let i=0;i<10;i++)M._tx_free(held.pop());
   },1);
   assert.ok(held.length,'MAP fallback memory pressure was not applied');
   assert.equal(Buffer.compare(fallback.bytes,map),0);
   assert.equal(fallback.sourceOffsets.filter((offset,i)=>i&&offset<fallback.sourceOffsets[i-1]).length,1);
   t.diagnostic(`MAP fallback held allocations: ${held.length}; output events: ${fallback.outputEvents}`);
   check(M._terra_world_stream_close(fallback.id));
  }finally{growthLimit=167772160;for(const p of held)M._tx_free(p);}
  const baseline=M._tx_native_heap_used();
  check(M._terra_world_stream_operation_begin(world,str('render_lit_map'),str('{}'),hp));
  const cancelId=M.HEAPU32[hp/4];
  assert.ok(pump(cancelId,null,null,10,true).headerStep>=0);
  check(M._terra_world_stream_cancel(cancelId));check(M._terra_world_stream_close(cancelId));
  assert.equal(M._tx_native_heap_used(),baseline,'cancel must release staged MAP bytes');
 }
 assert.notEqual(M._terra_world_stream_operation_begin(world,str('header_patch'),str(JSON.stringify({patch:{maxTilesX:64,maxTilesY:1}})),hp),0);
 assert.equal(M.HEAPU32[hp/4],0);
 for(const [name,request]of [['batch_update_tiles',{rules:[{where:{type:1},patch:{type:2}}]}],['header_patch',{patch:{worldName:'Streamed'}}],['save',{}]]){
  const r=streamed(name,request);assert.equal(r.kind,0);sources.set(2,r.bytes);check(M._terra_world_stream_adopt(r.id,2,hp));world=M.HEAPU32[hp/4];check(M._terra_world_stream_close(r.id));
 }
 check(M._terra_world_close(world));for(const p of ptrs)M._tx_free(p);
});
