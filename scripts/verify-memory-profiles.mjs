import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const require=createRequire(import.meta.url), MiB=1024*1024;
const [mpDir,h5Dir]=process.argv.slice(2);
assert.ok(mpDir,'Usage: node verify-memory-profiles.mjs <mp-build> [h5-build]');
// Decode the actual Wasm memory section, not manifest claims or wrapper strings.
function memory(bytes){
 assert.deepEqual([...bytes.subarray(0,8)],[0,97,115,109,1,0,0,0]);
 let o=8;
 function u32(){let n=0,s=0;for(;;){assert.ok(o<bytes.length);const b=bytes[o++];n+=(b&127)*2**s;if(!(b&128))return n;s+=7;assert.ok(s<35);}}
 let result;
 while(o<bytes.length){const id=bytes[o++],size=u32(),end=o+size;assert.ok(end<=bytes.length);
  if(id===5){assert.equal(u32(),1);const flags=u32();assert.equal(flags,1,'bounded, unshared memory32');result={initialPages:u32(),maxPages:u32()};assert.equal(o,end);}
  o=end;
 }
 assert.ok(result,'module must define bounded memory');return result;
}
async function check(dir,profile){
 dir=path.resolve(dir);const bytes=fs.readFileSync(path.join(dir,'terrax_world_wasm_web.wasm'));
 const cap=profile==='h5'?512:160, decoded=memory(bytes);
 assert.deepEqual(decoded,{initialPages:256,maxPages:cap*16});
 const manifest=JSON.parse(fs.readFileSync(path.join(dir,'terra.manifest.json')));
 assert.equal(manifest.memory.initialBytes,16*MiB);assert.equal(manifest.memory.maxBytes,cap*MiB);
 assert.equal(manifest.abi.circuitWorld.streamingTwld,false);
 let limit=32*MiB,calls=0;
 const M=await require(path.join(dir,'terrax_world_wasm_web.js'))({wasmBinary:bytes,memoryGrowthLimit:()=>{calls++;return limit;}});
 assert.equal(M.HEAPU8.byteLength,16*MiB);
 assert.equal(M._tx_native_heap_used(),0);
 const sentinel=M._tx_malloc(65536);assert.ok(sentinel);M.HEAPU8.fill(0xA5,sentinel,sentinel+65536);
 const used=M._tx_native_heap_used(), initial=M.HEAPU8.length;
 assert.equal(M._tx_malloc(40*MiB),0,'caller budget refuses growth');
 assert.ok(calls>0);assert.equal(M.HEAPU8.length,initial);assert.equal(M._tx_native_heap_used(),used);
 function intact(){assert.ok(M.HEAPU8.subarray(sentinel,sentinel+65536).every(x=>x===0xA5),'existing data survives rejected and successful growth');}
 intact();limit=256*MiB;
 const large=M._tx_malloc(170*MiB);
 if(profile==='h5'){
  assert.ok(large,'H5 must allocate beyond old 160 MiB ceiling');
  assert.ok(M.HEAPU8.length>160*MiB);assert.ok(M.HEAPU8.length<=limit);
  M.HEAPU8[large]=0x5A;M.HEAPU8[large+170*MiB-1]=0x3C;intact();
  limit=M.HEAPU8.length;const before=M.HEAPU8.length,beforeUsed=M._tx_native_heap_used();
  assert.equal(M._tx_malloc(100*MiB),0,'tightened caller budget blocks extra growth');
  assert.equal(M.HEAPU8.length,before);assert.equal(M._tx_native_heap_used(),beforeUsed);
  assert.equal(M.HEAPU8[large],0x5A);assert.equal(M.HEAPU8[large+170*MiB-1],0x3C);intact();
  M._tx_free(large);
 }else{assert.equal(large,0,'MP remains bounded at 160 MiB despite a 256 MiB caller grant');assert.equal(M.HEAPU8.length,initial);intact();}
 const retained=M.HEAPU8.length;M._tx_free(sentinel);
 assert.equal(M._tx_native_heap_used(),0);assert.equal(M._tx_bridge_heap_used(),0);
 assert.equal(M.HEAPU8.length,retained,'free releases native allocations, not linear memory');
 console.log(JSON.stringify({profile,...decoded,retainedHeapBytes:retained,nativeUsedAfterFree:0,callerBudgetChecks:calls,passed:true}));
 return manifest;
}
// Fixed known budget, fresh Node process/module: never probe a device's OOM
// boundary and never modify file-input or stream business limits.
async function checkBeyond200MB(dir){
 dir=path.resolve(dir);const bytes=fs.readFileSync(path.join(dir,'terrax_world_wasm_web.wasm'));
 assert.deepEqual(memory(bytes),{initialPages:256,maxPages:8192});
 let limit=384*MiB,calls=0;
 const M=await require(path.join(dir,'terrax_world_wasm_web.js'))({wasmBinary:bytes,memoryGrowthLimit:()=>{calls++;return limit;}});
 assert.equal(M.HEAPU8.length,16*MiB);assert.equal(M._tx_native_heap_used(),0);assert.equal(M._tx_bridge_heap_used(),0);
 const sentinel=M._tx_malloc(65536);assert.ok(sentinel);M.HEAPU8.fill(0xA5,sentinel,sentinel+65536);
 const size=300*MiB,large=M._tx_malloc(size);assert.ok(large,'H5 must admit known 300 MiB allocation under 384 MiB caller grant');
 assert.equal(M._tx_bridge_heap_used(),size+65536,'tx_malloc uses the bridge allocation domain');assert.ok(M.HEAPU8.length>200000000);assert.ok(M.HEAPU8.length<=limit);assert.ok(calls>0);
 const offsets=[0,Math.floor(size/2),size-1];for(const offset of offsets)M.HEAPU8[large+offset]=0x3C;
 function intact(){assert.ok(M.HEAPU8.subarray(sentinel,sentinel+65536).every(x=>x===0xA5));for(const offset of offsets)assert.equal(M.HEAPU8[large+offset],0x3C);}
 intact();const retained=M.HEAPU8.length,used=M._tx_native_heap_used(),bridgeUsed=M._tx_bridge_heap_used();limit=retained;
 assert.equal(M._tx_malloc(64*MiB),0,'tightened caller grant refuses further expansion');
 assert.equal(M.HEAPU8.length,retained);assert.equal(M._tx_native_heap_used(),used);assert.equal(M._tx_bridge_heap_used(),bridgeUsed);intact();
 M._tx_free(large);M._tx_free(sentinel);assert.equal(M._tx_native_heap_used(),0);assert.equal(M._tx_bridge_heap_used(),0);
 assert.equal(M.HEAPU8.length,retained,'free does not shrink linear memory');
 console.log(JSON.stringify({profile:'h5-beyond-200MB',allocationDomain:'bridge',allocationBytes:size,callerGrantBytes:384*MiB,retainedHeapBytes:retained,nativeUsedAfterFree:0,bridgeUsedAfterFree:0,callerBudgetChecks:calls,passed:true}));
}
if(mpDir==='--h5-large-only'){
 assert.ok(h5Dir,'H5 build directory required');await checkBeyond200MB(h5Dir);
}else{
 const mp=await check(mpDir,'mp');
 if(h5Dir){const h5=await check(h5Dir,'h5');assert.equal(h5.sourceCommit,mp.sourceCommit);assert.deepEqual(h5.abi,mp.abi);assert.deepEqual(h5.targets.node,mp.targets.node);console.log('Same source/ABI; Node artifacts unchanged.');
  execFileSync(process.execPath,[fileURLToPath(import.meta.url),'--h5-large-only',path.resolve(h5Dir)],{stdio:'inherit'});
 }
}
