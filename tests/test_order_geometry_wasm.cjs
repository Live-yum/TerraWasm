'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {makeDriver,makeCircuitWorld}=require('./helpers/circuit-world');
const cells=[...Array.from({length:20},(_,i)=>({x:i+2,y:6,wires:1})),
 {x:12,y:6,type:419,wires:1},{x:12,y:7,type:420,wires:4},{x:22,y:6,type:419,wires:1},{x:22,y:7,type:420,wires:2},
 ...Array.from({length:4},(_,i)=>({x:i+23,y:7,wires:2})),{x:27,y:7,type:419,wires:2}];
test('real Wasm ordering source reads pause, resume, and cancel without duplicate effects',async()=>{
 const d=await makeDriver();const bytes=makeCircuitWorld(cells,36,16);
 try {
  d.open(bytes);d.command(2,{x:2,y:6,mask:1});const expected=[d.cell(22,6),d.cell(22,7),d.cell(27,7)];
  d.open(bytes);assert.equal(d.start(2,{x:2,y:6,mask:1}),0);let reads=0;
  for(let i=0;i<100000;i++){
   const e=d.event(false,0xffffffff);
   if(e[1]===4)break;
   if(e[1]===1&&e[2]===1){++reads;for(let n=0;n<3;n++)assert.deepEqual(d.event(false,0xffffffff),e);}
   if(e[1]>=1&&e[1]<=3)d.respond(e);
   assert.ok(i<99999);
  }
  assert.ok(reads>0,'ordering reads immutable WLD source during trip');
  assert.deepEqual([d.cell(22,6),d.cell(22,7),d.cell(27,7)],expected);
  d.open(bytes);const before=[d.cell(22,6),d.cell(22,7),d.cell(27,7)];
  assert.equal(d.start(2,{x:2,y:6,mask:1}),0);let cancelled=false;
  for(let i=0;i<100000;i++){
   const e=d.event(false,0xffffffff);
   if(e[1]===1&&e[2]===1){d.cancel();cancelled=true;break;}
   assert.notEqual(e[1],4);if(e[1]>=1&&e[1]<=3)d.respond(e);
  }
  assert.ok(cancelled);assert.deepEqual([d.cell(22,6),d.cell(22,7),d.cell(27,7)],before);
  d.command(2,{x:2,y:6,mask:1});assert.deepEqual([d.cell(22,6),d.cell(22,7),d.cell(27,7)],expected);
 } finally {d.dispose();}
});
