'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),path=require('node:path');
const {makeCircuitWorld,makeDriver}=require('../helpers/circuit-world.js');
function fixture({wall=false,flipped=false}={}) {
 const cells=[];
 for(let x=0;x<4;x++)for(let y=0;y<3;y++)cells.push({x:10+x,y:10+y,type:209,fx:216+x*18,fy:y*18,wires:x===2&&y===2?1:0});
 cells.push({x:7,y:12,type:136,wires:1});for(let x=8;x<12;x++)cells.push({x,y:12,wires:1});
 for(const x of [16,19]){cells.push({x,y:12,type:442,fx:flipped?0:22,wires:2});cells.push({x,y:13,type:419,wires:2});}
 if(wall)cells.push({x:15,y:12,type:1});
 return makeCircuitWorld(cells,80,80);
}
const fire=d=>d.command(2,{x:7,y:12,mask:1,count:1,flags:1});
test('601 travels 31 x 3 px per tick and each 442 fires on entry only',async()=>{
 const d=await makeDriver();try{d.open(fixture());fire(d);assert.equal(d.cell(16,13).fx,0);d.command(3,{count:1});assert.equal(d.cell(16,13).fx,18);assert.equal(d.cell(19,13).fx,0);d.command(3,{count:1});assert.equal(d.cell(19,13).fx,18);d.command(3,{count:10});assert.equal(d.cell(16,13).fx,18);assert.equal(d.cell(19,13).fx,18);}finally{d.dispose();}
});
test('station 30-tick CheckMech blocks repeated wire shots',async()=>{
 const d=await makeDriver();try{d.open(fixture());fire(d);fire(d);d.command(3,{count:2});assert.equal(d.cell(16,13).fx,18);d.command(3,{count:28});fire(d);d.command(3,{count:2});assert.equal(d.cell(16,13).fx,0);}finally{d.dispose();}
});
test('solid terrain stops the projectile before downstream pads',async()=>{
 const d=await makeDriver();try{d.open(fixture({wall:true}));fire(d);d.command(3,{count:10});assert.equal(d.cell(16,13).fx,0);assert.equal(d.cell(19,13).fx,0);}finally{d.dispose();}
});
test('cancel restores active projectile position, lifetime, lamps and clock',async()=>{
 const d=await makeDriver();try{d.open(fixture());fire(d);assert.equal(d.start(3,{count:50}),0);for(let i=0;i<8;i++){const e=d.event(false,1);if(e[1]>=1&&e[1]<=3)d.respond(e);}d.cancel();assert.equal(d.stats()[18],0);assert.equal(d.cell(16,13).fx,0);d.command(3,{count:2});assert.equal(d.cell(16,13).fx,18);assert.equal(d.cell(19,13).fx,18);}finally{d.dispose();}
});
function spearFixture({wall=true}={}) {
 const cells=[{x:7,y:12,type:136,wires:1},{x:8,y:12,wires:1},{x:9,y:12,type:137,fx:72,fy:72,wires:1},{x:11,y:12,type:442,fx:66,wires:2},{x:11,y:13,type:419,wires:2}];
 if(wall)cells.push({x:12,y:12,type:1});
 return makeCircuitWorld(cells,80,80);
}
test('186 spear travels once per tick, enters side pad, reverses at solid and retires',async()=>{
 const d=await makeDriver();try{d.open(spearFixture());fire(d);assert.equal(d.cell(11,13).fx,0);d.command(3,{count:2});assert.equal(d.cell(11,13).fx,18);d.command(3,{count:20});assert.equal(d.cell(11,13).fx,18);fire(d);d.command(3,{count:2});assert.equal(d.cell(11,13).fx,18);d.command(3,{count:66});fire(d);d.command(3,{count:2});assert.equal(d.cell(11,13).fx,0);}finally{d.dispose();}
});

function timerProjectileFixture(timerIsSource) {
 const cells=[];
 for(let x=0;x<4;x++)for(let y=0;y<3;y++)cells.push({x:10+x,y:10+y,type:209,fx:216+x*18,fy:y*18,wires:x===2&&y===2?1:0});
 cells.push({x:7,y:12,type:timerIsSource?144:136,wires:1});
 for(let x=8;x<12;x++)cells.push({x,y:12,wires:1});
 cells.push({x:16,y:12,type:442,fx:22,wires:2});
 if(timerIsSource)cells.push({x:16,y:13,type:419,wires:2});
 else cells.push({x:16,y:13,type:144,fx:72,wires:2},{x:16,y:14,type:419,wires:2});
 return makeCircuitWorld(cells,80,80);
}

// Main.DoUpdateInWorld_Inner runs UpdateWorld_Projectiles before
// UpdateWorld_WorldGenAndInvasion -> WorldGen.UpdateWorld -> Wiring.UpdateMech.
// This is observable in both directions, even without any NPC/player simulation.
test('a timer-created 601 first moves on the following game tick',async()=>{
 const d=await makeDriver();try{
  d.open(timerProjectileFixture(true));fire(d);
  d.command(3,{count:60});
  assert.equal(d.cell(16,13).fx,0,'UpdateMech fires after this tick has already advanced projectiles');
  d.command(3,{count:1});
  assert.equal(d.cell(16,13).fx,18,'the shot reaches the first pad on tick 61');
 }finally{d.dispose();}
});

test('a projectile-enabled timer counts down during the same game tick',async()=>{
 const d=await makeDriver();try{
  d.open(timerProjectileFixture(false));fire(d);
  d.command(3,{count:1});
  assert.equal(d.cell(16,13).fy,18);
  assert.equal(d.cell(16,14).fx,18,'the pad input enables the timer and toggles its output lamp');
  d.command(3,{count:13});
  assert.equal(d.cell(16,14).fx,18);
  d.command(3,{count:1});
  assert.equal(d.cell(16,14).fx,0,'the timer registered during projectiles is included in this tick\'s UpdateMech snapshot');
 }finally{d.dispose();}
});

test('442 pressure pads detect boundary contact using Utils.FloatIntersect',async()=>{
 const cells=[];
 for(let x=0;x<4;x++)for(let y=0;y<3;y++)cells.push({x:10+x,y:10+y,type:209,fx:216+x*18,fy:y*18,wires:x===2&&y===2?1:0});
 cells.push({x:7,y:12,type:136,wires:1});
 for(let x=8;x<12;x++)cells.push({x,y:12,wires:1});
 // The bottom-oriented pad occupies y=182..192. The shot's top is exactly
 // y=192, so it touches the pad without overlapping its interior.
 cells.push({x:16,y:11,type:442,fx:0,wires:2},{x:16,y:10,type:419,wires:2});
 const d=await makeDriver();try{
  d.open(makeCircuitWorld(cells,80,80));fire(d);d.command(3,{count:1});
  assert.equal(d.cell(16,10).fx,18,'touching the pad boundary is an entry in the source game');
  d.command(3,{count:3});
  assert.equal(d.cell(16,10).fx,18,'remaining on the boundary does not retrigger');
 }finally{d.dispose();}
});
