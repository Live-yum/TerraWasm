'use strict';
// Node24: --import ./viewer-app/scripts/node-alias-register.mjs; optional --preview / --worst-pair.
// Direct positioned host file I/O around the real 160 MiB Web module.
// This measures native streaming boundaries; actual wx/session I/O is tested in viewer-app.
// Managed estimate includes linear memory, dense canvas, maps and IO; excludes host runtime overhead.
const fs=require('fs'),assert=require('assert').strict,os=require('os'),{performance}=require('perf_hooks');
const {getPrimaryWorldPath}=require('../tests/helpers/fixtures');
(async()=>{
 const M=await require('../build/terrax_world_wasm_web.js')({wasmBinary:fs.readFileSync(__dirname+'/../build/terrax_world_wasm_web.wasm'),memoryGrowthLimit:()=>140000000});
 const width=8400,height=2400,cols=132,rows=38,paletteCount=65536;
 const chunks=[];for(let cy=0;cy<rows;cy++)for(let cx=0;cx<cols;cx++){const b=new Uint16Array(4096);let used=0;for(let y=0;y<64&&cy*64+y<height;y++)for(let x=0;x<64&&cx*64+x<width;x++){b[y*64+x]=1+((cx*64+x)*73+(cy*64+y)*151)%65535;used++;}chunks.push({b,used});}
 const maps=Buffer.alloc(paletteCount*12);let mappingMs=0;
 if(process.argv.includes('--worst-pair')){
  for(let i=1;i<paletteCount;i++){maps[i*12+3]=255;maps.writeUInt16LE(1,i*12+4);maps.writeUInt16LE(1,i*12+6);maps[i*12+8]=i%31;maps[i*12+9]=(i>>5)%31;maps[i*12+10]=4;}
 }else{
  const {pathToFileURL}=require('url'),path=require('path');
  const {resolveStablePaletteMappings}=await import(pathToFileURL(path.resolve(__dirname,'../../viewer-app/features/pixel-art/services/stable-pixel-mapping.js')));
  const {buildWasmOverridesFromMappingColors}=await import(pathToFileURL(path.resolve(__dirname,'../../viewer-app/features/pixel-art/services/pixel-mapping.js')));
  const colors=Array.from({length:65535},(_,i)=>'#'+(((i+1)*2654435761)>>>0&0xffffff).toString(16).padStart(6,'0'));
  const began=performance.now(),entries=buildWasmOverridesFromMappingColors(resolveStablePaletteMappings(colors));mappingMs=performance.now()-began;assert.equal(entries.length,65535);
  entries.forEach((e,j)=>{let i=j+1;maps[i*12]=e.r;maps[i*12+1]=e.g;maps[i*12+2]=e.b;maps[i*12+3]=255;maps.writeUInt16LE(e.tileType||0,i*12+4);maps.writeUInt16LE(e.wallType||0,i*12+6);maps[i*12+8]=e.tileColor||0;maps[i*12+9]=e.wallColor||0;maps[i*12+10]=e.skip?3:e.active===false?0:e.activeMode??(e.tileType!=null?4:e.wallType?2:0);});
 }
 const alloc=n=>{const p=M._tx_malloc(n);assert(p);return p;},hp=alloc(4),ep=alloc(48),io=alloc(1048576),mp=alloc(maps.length),sp=alloc(40);
 M.HEAPU8.set(maps,mp);const source=fs.openSync(getPrimaryWorldPath(),'r'),output=fs.openSync(__dirname+'/../build/stream-8400.wld','w+'),registry=new Map([[1,source],[2,output]]);let outputTarget=output;let readBytes=0,writeBytes=0,readMs=0,writeMs=0,kernelMs=0,peak=0;
 function pump(id,pixel){for(let i=0;i<10000000;i++){let t=performance.now();assert.equal(M._terra_world_stream_step(id,64,ep),0);kernelMs+=performance.now()-t;const e=Array.from(M.HEAPU32.subarray(ep/4,ep/4+12));peak=Math.max(peak,M.HEAPU8.length+chunks.length*8192+maps.length+1048576);
 if(!e[1])continue;if(e[1]===1){t=performance.now();const n=fs.readSync(registry.get(e[2]),M.HEAPU8.subarray(io,io+e[4]),0,e[4],e[3]);assert.equal(n,e[4]);readMs+=performance.now()-t;readBytes+=n;assert.equal(M._terra_world_stream_supply_source(id,e[2],e[3],io,n),0);}
 else if(e[1]===2){let n=0;for(let cy=0;cy<rows;cy++){let c=chunks[cy*cols+e[6]/64],p=io+n*8200;const d=new DataView(M.HEAPU8.buffer,p,8200);d.setUint16(0,e[6]/64,true);d.setUint16(2,cy,true);d.setUint16(4,c.used,true);d.setUint16(6,0,true);M.HEAPU8.set(new Uint8Array(c.b.buffer),p+8);n++;}assert.equal(M._terra_world_stream_supply_pixels(id,io,n*8200,n),0);}
 else if(e[1]===3){t=performance.now();assert.equal(fs.writeSync(outputTarget,M.HEAPU8.subarray(e[5],e[5]+e[4]),0,e[4],e[3]),e[4]);writeMs+=performance.now()-t;writeBytes+=e[4];assert.equal(M._terra_world_stream_ack_output(id),0);}
 else if(e[1]===4)return e[10];else assert.fail('event');}assert.fail('steps');}
 let t=performance.now();assert.equal(M._terra_world_stream_open_begin(1,fs.fstatSync(source).size,hp),0);let id=M.HEAPU32[hp/4];pump(id);assert.equal(M._terra_world_stream_adopt(id,1,hp),0);let world=M.HEAPU32[hp/4];M._terra_world_stream_close(id);let openMs=performance.now()-t;
 readBytes=writeBytes=kernelMs=readMs=writeMs=0;t=performance.now();M.HEAPU32.set([1,0,0,width,height,mp,paletteCount,0,0,0],sp/4);assert.equal(M._terra_world_stream_pixel_begin(world,sp,hp),0);id=M.HEAPU32[hp/4];let size=pump(id,true);fs.fsyncSync(output);assert.equal(M._terra_world_stream_adopt(id,2,hp),0);world=M.HEAPU32[hp/4];M._terra_world_stream_close(id);const totalMs=performance.now()-t;
 console.log(JSON.stringify({cpu:os.cpus()[0].model,node:process.version,scenario:process.argv.includes('--worst-pair')?'worst-pair':'actual-stable-nearest',mappingMs,openMs,totalMs,kernelMs,readMs,writeMs,readBytes,writeBytes,outputBytes:size,linear:M.HEAPU8.length,managedPeakEstimate:peak,canvasBytes:chunks.length*8192,rss:process.memoryUsage().rss},null,2));assert(peak<=200000000);
 if(process.argv.includes('--preview')){
  readBytes=writeBytes=kernelMs=readMs=writeMs=0;const name=alloc(32),request=alloc(3);
  M.HEAPU8.set(Buffer.from('render_preview_png\0'),name);M.HEAPU8.set(Buffer.from('{}\0'),request);
  outputTarget=fs.openSync(__dirname+'/../build/stream-preview.png','w+');const began=performance.now();
  assert.equal(M._terra_world_stream_operation_begin(world,name,request,hp),0);id=M.HEAPU32[hp/4];const pngBytes=pump(id);
  fs.fsyncSync(outputTarget);M._terra_world_stream_close(id);fs.closeSync(outputTarget);M._tx_free(name);M._tx_free(request);
  console.log(JSON.stringify({pngMs:performance.now()-began,pngKernelMs:kernelMs,pngReadBytes:readBytes,pngBytes,linear:M.HEAPU8.length,managedPeakEstimate:peak},null,2));assert(peak<=200000000);
 }
 M._terra_world_close(world);for(const p of [hp,ep,io,mp,sp])M._tx_free(p);fs.closeSync(source);fs.closeSync(output);
})().catch(e=>{console.error(e);process.exitCode=1;});
