'use strict';
// Synthetic, independently encoded modern WLDs; no distributed game assets.
const fs = require('node:fs');
const path = require('node:path');
const {makeSectionedWorld} = require('./helpers/sectioned-world');
function fixture(width,height,noise) {
 const base=makeSectionedWorld(139,{worldName:'stream-contract'}),count=base.readUInt16LE(24),start=base.readUInt32LE(26);
 const header=Buffer.from(base.subarray(start));
 const dimensions=Buffer.alloc(8);dimensions.writeInt32LE(500);dimensions.writeInt32LE(1000,4);
 const at=header.indexOf(dimensions);if(at<0)throw Error('missing independent fixture dimensions');
 header.writeInt32LE(height,at);header.writeInt32LE(width,at+4);
 const short=noise==='short';
 if(short&&height%40)throw Error('short-column fixture needs height divisible by 40');
 const tiles=Buffer.alloc(width*(short?height/40*3:noise?height*2:4));let pos=0;
 for(let x=0;x<width;x++){
  if(short)for(let y=0;y<height;y+=40){tiles[pos++]=66;tiles[pos++]=1+((x+y/40)&1);tiles[pos++]=39;}
  else if(noise)for(let y=0;y<height;y++){tiles[pos++]=2;tiles[pos++]=1+((x+y)&1);}
  else{tiles[pos++]=130;tiles[pos++]=1+(x&1);tiles.writeUInt16LE(height-1,pos);pos+=2;}
 }
 const footer=Buffer.concat([Buffer.from([1,15]),Buffer.from('stream-contract'),Buffer.from([1,0,0,0])]);
 const sections=[header,tiles,Buffer.from([0,0,40,0]),Buffer.alloc(2),Buffer.alloc(1),Buffer.alloc(4),footer];
 const format=Buffer.from(base.subarray(0,start));format.writeUInt16LE(3,26+count*4);
 let offset=format.length;sections.forEach((s,i)=>{format.writeUInt32LE(offset,26+i*4);offset+=s.length;});
 return Buffer.concat([format,...sections]);
}
if(require.main===module){const dir=process.argv[2];fs.mkdirSync(dir,{recursive:true});for(const [name,w,h,n] of [['high',8400,2400,false],['noise',8400,257,true],['small',7,270,true],['markers',513,300,true],['short-columns',8400,2400,'short'],['marker-wide',513,385,false],['marker-narrow',7,1000,false]])fs.writeFileSync(path.join(dir,name+'.wld'),fixture(w,h,n));
 const {makeCircuitWorld}=require('./helpers/circuit-world');const cells=new Map();
 const cell=(x,y,v)=>cells.set(`${x},${y}`,{...(cells.get(`${x},${y}`)||{}),x,y,...v});
 for(let x=3;x<=9;x++)cell(x,4,{wires:1});for(let y=2;y<=8;y++)cell(6,y,{wires:y===4?3:2});
 for(let x=0;x<2;x++)for(let y=0;y<2;y++)cell(12+x,10+y,{type:132,fx:x*18,fy:y*18,paint:7,wall:2,wallPaint:9,wires:x===0&&y===0?1:x===1&&y===1?2:0});
 cell(11,10,{wires:1});cell(14,11,{wires:2});
 cell(20,5,{type:419,wires:1});cell(20,6,{type:419});cell(20,7,{type:420,wires:2});
 for(let x=24;x<=28;x++)cell(x,10,{wires:1});for(let y=8;y<=12;y++)cell(26,y,{wires:1});cell(26,10,{type:424});
 cell(30,25,{type:1,wires:2,paint:4,wall:3,wallPaint:6});
 fs.writeFileSync(path.join(dir,'circuit-fragments.wld'),makeCircuitWorld([...cells.values()]));
 const objects=require('./helpers/circuit-objects').fixture();fs.writeFileSync(path.join(dir,'circuit-objects.wld'),objects.world);fs.writeFileSync(path.join(dir,'circuit-objects.cob'),objects.companion);fs.writeFileSync(path.join(dir,'circuit-objects.geometry'),require('./helpers/circuit-objects').words(objects.geometry.flat()));
 fs.writeFileSync(path.join(dir,'circuit-objects-old.wld'),makeCircuitWorld([],100,32));
}
module.exports={fixture};
