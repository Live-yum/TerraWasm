'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const test=require('node:test');
const {getPrimaryWorldPath}=require('./helpers/fixtures');
const {minimalTxci}=require('./helpers/minimal-txci');

function sections(b) {
  const at=b.readUInt32LE(0)>=135?26:6,n=b.readUInt16LE(at-2);
  const starts=Array.from({length:n},(_,i)=>b.readUInt32LE(at+4*i));
  return {at,starts,parts:starts.map((s,i)=>b.subarray(s,starts[i+1]||b.length))};
}
function replaceSections(b,replacements) {
  const {at,starts,parts}=sections(b),format=Buffer.from(b.subarray(0,starts[0]));
  let offset=format.length;
  const result=parts.map((part,i)=>replacements[i]||part);
  result.forEach((part,i)=>{format.writeUInt32LE(offset,at+4*i);offset+=part.length;});
  return Buffer.concat([format,...result]);
}
function records(section) {
  const values=[0,2,4].map(x=>{
    const coordinates=Buffer.alloc(section===5?9:8);
    if(section===5){coordinates[0]=x===4?8:1;coordinates.writeUInt32LE(x+1,1);coordinates.writeUInt16LE(x,5);}
    else coordinates.writeInt32LE(x,0);
    if(section===2)return Buffer.concat([coordinates,Buffer.from([1,65,1,0,0,0,1,0,1,0,0,0,0])]);
    if(section===3)return Buffer.concat([Buffer.from([1,65]),coordinates]);
    return Buffer.concat([coordinates,Buffer.from([1,0,0,1,0])]);
  });
  const count=Buffer.alloc(section===5?4:2);count[0]=3;
  return {values,bytes:Buffer.concat([count,...values]),expected:Buffer.concat([Buffer.from(count.map((v,i)=>i===0?2:v)),...values.slice(1)])};
}
async function runtime() {
  if(!process.env.PIXEL_ART_WEB_ARTIFACT)return require('../build/terrax_world_wasm.js')();
  const js=path.resolve(process.env.PIXEL_ART_WEB_ARTIFACT);
  return require(js)({wasmBinary:fs.readFileSync(js.replace(/\.js$/,'.wasm'))});
}
for (const route of process.env.PIXEL_ART_WEB_ARTIFACT ? ['fast'] : ['fast','single','bulk','rgba'])
test(`${route}: empty clears all tile fields and partial-object metadata; skip and adjacent objects stay byte-identical`,async()=>{
  const M=await runtime(),allocations=[];
  const alloc=data=>{const p=M._tx_malloc(typeof data==='number'?data:data.length);assert.ok(p);allocations.push(p);if(typeof data!=='number')M.HEAPU8.set(data,p);return p;};
  const str=s=>alloc(Buffer.from(s+'\0'));
  const hp=alloc(4),size=alloc(8);
  function error(){M._terra_info_get_last_error_json(0,0n,size);const n=M.HEAPU32[size>>2],p=alloc(n);M._terra_info_get_last_error_json(p,BigInt(n),size);return Buffer.from(M.HEAPU8.subarray(p,M.HEAPU8.indexOf(0,p))).toString();}
  function open(bytes){const p=alloc(bytes),task=M._terra_world_open_begin(p,bytes.length);assert.ok(task,error());let status=10;while(status===10)status=M._terra_world_open_step(task,100);assert.equal(status,0,error());assert.equal(M._terra_world_open_finish(task,hp),0);M._terra_world_task_close(task);return M.HEAPU32[hp>>2];}
  function json(handle,name){const key=str(name);assert.equal(M._terra_section_get_json(handle,key,0,0n,size),0);const len=M.HEAPU32[size>>2],p=alloc(len);assert.equal(M._terra_section_get_json(handle,key,p,BigInt(len),size),0);return JSON.parse(Buffer.from(M.HEAPU8.subarray(p,M.HEAPU8.indexOf(0,p))).toString());}
  function commit(handle){assert.equal(M._terra_world_commit_to_buffer(handle,0,0,size,hp),0);const len=M.HEAPU32[size>>2],p=alloc(len);assert.equal(M._terra_world_commit_to_buffer(handle,p,len,size,hp),0);return {bytes:Buffer.from(M.HEAPU8.subarray(p,p+len)),handle:M.HEAPU32[hp>>2]};}
  const original=fs.readFileSync(getPrimaryWorldPath());let handle=open(original);
  const header=json(handle,'header');M._terra_world_close(handle);
  const dirty=Buffer.from([15,15,63,30,1,7,2,3,180]);
  const rest=Buffer.alloc(3);rest[0]=128;rest.writeUInt16LE(header.maxTilesY-2,1);
  const columns=Array.from({length:header.maxTilesX},()=>Buffer.concat([dirty,rest]));
  columns[4]=Buffer.concat([Buffer.from([2,88,0,0,0,0]),rest]);
  const chest=records(2),sign=records(3),entity=records(5);
  const plates=Buffer.alloc(28);plates.writeUInt32LE(3,0);plates.writeInt32LE(1,4);plates.writeInt32LE(2,12);plates.writeInt32LE(4,20);
  handle=open(replaceSections(original,{1:Buffer.concat(columns),2:chest.bytes,3:sign.bytes,5:entity.bytes,6:plates}));
  const txci=minimalTxci(),palette=Buffer.from([0,0,0,0,8,9,10,255,11,12,13,255]),maps=Buffer.alloc(24);
  maps.set(palette.subarray(4,8),0);maps.set(palette.subarray(8,12),12);maps[22]=3;
  const indices=Buffer.alloc(8192);indices.writeUInt16LE(1,0);indices.writeUInt16LE(2,2);indices.writeUInt16LE(2,4);
  const record=Buffer.concat([Buffer.from([0,0,0,0,3,0,0,0]),indices]);
  if (route==='rgba') {
    const pixels=Buffer.concat([palette.subarray(4,8),palette.subarray(8,12),palette.subarray(8,12)]);
    assert.equal(M._txw_apply_pixel_art(handle,alloc(pixels),pixels.length,3,1,alloc(txci),txci.length,1,0,0,0,alloc(maps),2),0);
  } else {
    assert.equal(M._txw_begin_pixel_art_indexed(handle,1,0,3,1,alloc(palette),3,alloc(txci),txci.length,0,0,alloc(maps),2,0),0);
    if (route==='single') assert.equal(M._txw_add_pixel_art_chunk(handle,0,0,alloc(indices),4096,3),0);
    else assert.equal(M[route==='bulk'?'_txw_add_pixel_art_chunks_bulk':'_txw_add_pixel_art_chunks_bulk_fast'](handle,alloc(record),record.length,1),0);
  }
  const output=commit(handle);handle=output.handle;
  const parts=sections(output.bytes).parts;
  assert.deepEqual(parts[2],chest.expected);assert.deepEqual(parts[3],sign.expected);assert.deepEqual(parts[5],entity.expected);
  const keptPlates=Buffer.concat([Buffer.from([2,0,0,0]),plates.subarray(12)]);
  assert.deepEqual(parts[6],keptPlates);
  // Column 0 unchanged, column 1 empty, column 2 retains the entire wired/liquid/painted tile.
  assert.deepEqual(parts[1].subarray(0,dirty.length),dirty);
  const emptyAt=dirty.length+rest.length;
  assert.equal(parts[1][emptyAt],0);assert.equal(parts[1][emptyAt+1],128);assert.equal(parts[1].readUInt16LE(emptyAt+2),header.maxTilesY-2);
  assert.deepEqual(parts[1].subarray(emptyAt+4,emptyAt+4+dirty.length),dirty);
  // Only the dresser's third column is erased; nearby 2x2 sign/frame remain.
  assert.equal(M._txw_begin_pixel_art_indexed(handle,6,0,1,1,alloc(palette),3,alloc(txci),txci.length,0,0,alloc(maps),2,0),0);
  const third=Buffer.alloc(8200);third.writeUInt16LE(1,4);third.writeUInt16LE(1,8);
  assert.equal(M._txw_add_pixel_art_chunks_bulk_fast(handle,alloc(third),third.length,1),0);
  const dresserOutput=commit(handle);handle=dresserOutput.handle;
  const dresserParts=sections(dresserOutput.bytes).parts;
  assert.deepEqual(dresserParts[2],Buffer.concat([Buffer.from([1,0]),chest.values[1]]));
  assert.deepEqual(dresserParts[3],sign.expected);assert.deepEqual(dresserParts[5],entity.expected);
  function erase(x,y) {
    assert.equal(M._txw_begin_pixel_art_indexed(handle,x,y,1,1,alloc(palette),3,alloc(txci),txci.length,0,0,alloc(maps),2,0),0);
    assert.equal(M._txw_add_pixel_art_chunks_bulk_fast(handle,alloc(third),third.length,1),0);
    const saved=commit(handle);handle=saved.handle;return sections(saved.bytes).parts;
  }
  // Dead Cells jar is 1x2: its right neighbor is independent; lower half is not.
  assert.deepEqual(erase(5,0)[5],entity.expected);
  assert.deepEqual(erase(4,1)[5],Buffer.concat([Buffer.from([1,0,0,0]),entity.values[1]]));
  M._terra_world_close(handle);
  // Display-doll equipment/dye/misc masks encode six variable item payloads.
  const doll=Buffer.alloc(47);doll[0]=1;doll[4]=3;doll.writeInt32LE(1,5);doll.set([3,1,2,7],13);
  for(let at=17;at<47;at+=5)doll.set([1,0,0,1,0],at);
  handle=open(replaceSections(original,{1:Buffer.concat(columns),5:doll}));
  assert.deepEqual(erase(1,0)[5],Buffer.alloc(4));
  M._terra_world_close(handle);
  // An unsupported entity record must not partially adopt preceding chest/sign filters.
  const malformed=Buffer.alloc(13);malformed[0]=1;malformed[4]=255;
  handle=open(replaceSections(original,{1:Buffer.concat(columns),2:chest.bytes,3:sign.bytes,5:malformed}));
  assert.equal(M._txw_begin_pixel_art_indexed(handle,1,0,1,1,alloc(palette),3,alloc(txci),txci.length,0,0,alloc(maps),2,0),0);
  assert.equal(M._txw_add_pixel_art_chunks_bulk_fast(handle,alloc(third),third.length,1),0);
  for(let retry=0;retry<2;retry++) {
    assert.notEqual(M._terra_world_commit_to_buffer(handle,0,0,size,hp),0);
    assert.match(error(),/metadata.*malformed or unsupported/);
    assert.equal(json(handle,'chests').length,3);assert.equal(json(handle,'signs').length,3);
  }
  M._terra_world_close(handle);allocations.forEach(p=>M._tx_free(p));
});
