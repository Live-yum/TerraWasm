'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const path=require('node:path');
const create=require(path.resolve(process.env.TERRAX_HEADER_WASM || path.join(__dirname,'../build/terrax_world_wasm.js')));
const {makeSectionedWorld}=require('./helpers/sectioned-world');
test('bounded header ABI v1 preserves exact bytes, uint64 capacity and legacy cache across synthetic versions',async()=>{
 const M=await create();assert.equal(M._terra_header_json_abi_version(),1);
 const identity=JSON.parse(M.UTF8ToString(M._terra_build_info_json()));assert.equal(identity.headerJsonAbiVersion,1);
 for(const version of [88,179,293,326]){
  const bytes=makeSectionedWorld(version,{worldName:'long"\\\n世界'.repeat(128),seed:version===179?123:'seed"\\\0'.repeat(60),anglerWhoFinishedToday:['angler"\\\0'.repeat(80)],manifestJson:'manifest"\\\n'.repeat(800)});
  const input=M._tx_malloc(bytes.length),handle=M._tx_malloc(4),required=M._tx_malloc(8),section=M._tx_malloc(7);
  let h=0,output=0,legacy=0;
  try{
   M.HEAPU8.set(bytes,input);assert.equal(M._terra_world_open_from_buffer(input,bytes.length,handle),0);h=M.HEAPU32[handle>>>2];M.HEAPU8.set(Buffer.from('header\0'),section);
   assert.equal(M._terra_section_get_json(h,section,0,0n,required),0);const size=M.HEAPU32[required>>>2];
   assert.equal(M._terra_header_get_json(h,0,0n,required),0);assert.equal(M.HEAPU32[required>>>2],size);assert.equal(M.HEAPU32[(required>>>2)+1],0);
   output=M._tx_malloc(size+2);legacy=M._tx_malloc(size);M.HEAPU8.fill(165,output,output+size+2);
   assert.notEqual(M._terra_header_get_json(h,output+1,BigInt(size-1),required),0);
   assert(M.HEAPU8.slice(output,output+size+2).every(x=>x===165));
   assert.equal(M._terra_header_get_json(h,output+1,1n<<32n,required),0);
   assert.equal(M.HEAPU8[output],165);assert.equal(M.HEAPU8[output+size+1],165);assert.equal(M.HEAPU8[output+size],0);
   assert.equal(M._terra_section_get_json(h,section,legacy,BigInt(size),required),0);
   assert.deepEqual(M.HEAPU8.slice(output+1,output+size+1),M.HEAPU8.slice(legacy,legacy+size));
  }finally{if(h)M._terra_world_close(h);for(const p of [input,handle,required,section,output,legacy])if(p)M._tx_free(p);}
 }
});
