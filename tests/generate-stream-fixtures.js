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
if(require.main===module){const dir=process.argv[2];fs.mkdirSync(dir,{recursive:true});for(const [name,w,h,n] of [['high',8400,2400,false],['noise',8400,257,true],['small',7,270,true],['markers',513,300,true],['short-columns',8400,2400,'short'],['marker-wide',513,385,false],['marker-narrow',7,1000,false]])fs.writeFileSync(path.join(dir,name+'.wld'),fixture(w,h,n));}
module.exports={fixture};
