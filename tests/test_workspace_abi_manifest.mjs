import test from 'node:test'
import assert from 'node:assert/strict'
import {createHash} from 'node:crypto'
import {validateManifest,WORLD_WORKSPACE_EXPORTS,PIXEL_WORKSPACE_EXPORTS,PLAYER_WORKSPACE_EXPORTS} from '../scripts/generate-manifest.mjs'
const hash=x=>createHash('sha256').update(x).digest('hex')
function fixture(kind='all'){
 const names=['_terra_build_info_json',...kind!=='plr'?[...WORLD_WORKSPACE_EXPORTS,...PIXEL_WORKSPACE_EXPORTS]:[],...kind!=='wld'?PLAYER_WORKSPACE_EXPORTS:[]]
 const target=name=>({memory:{initialBytes:65536,maxBytes:65536},exports:[...names],exportHash:hash(names.join('\n')+'\n'),artifacts:[{role:'wrapper',path:`build/${name}.js`,bytes:1,sha256:'a'.repeat(64)},{role:'wasm',path:`build/${name}.wasm`,bytes:1,sha256:'b'.repeat(64)}]})
 const targets={node:target('node'),web:target('web')}
 const abi={version:1,requiredExports:[...names],exportHash:targets.web.exportHash}
 if(kind!=='plr')abi.worldWorkspace={version:1,checkpoint:true,rollback:true}
 if(kind!=='plr')abi.pixelWorkspace={version:1,blockSide:64,authoritative:true}
 if(kind!=='wld')abi.playerWorkspace={version:1,fieldPatches:true,rollbackJournal:true}
 return {version:1,artifactId:'terrax-world-web',sourceCommit:'a'.repeat(40),dirty:false,abi,memory:targets.web.memory,build:{compiler:'test fixture only',featureSet:kind,flags:{common:['test'],node:['test'],web:['test']}},targets,artifacts:targets.web.artifacts}
}
test('workspace capabilities validate across feature profiles and legacy absent declarations',()=>{
 for(const profile of ['all','wld','plr'])assert.doesNotThrow(()=>validateManifest(fixture(profile)))
 const m=fixture();delete m.abi.pixelWorkspace;delete m.abi.playerWorkspace;assert.doesNotThrow(()=>validateManifest(m))
})
test('producer rejects altered contracts, missing exports, and feature mismatches',()=>{
 for(const [key,fields] of [['worldWorkspace',['version','checkpoint','rollback']],['pixelWorkspace',['version','blockSide','authoritative']],['playerWorkspace',['version','fieldPatches','rollbackJournal']]]){
  for(const field of [...fields,'unexpected']){const m=fixture();m.abi[key][field]=null;assert.throws(()=>validateManifest(m),/workspace ABI/)}
 }
 for(const name of [...WORLD_WORKSPACE_EXPORTS,...PIXEL_WORKSPACE_EXPORTS,...PLAYER_WORKSPACE_EXPORTS]){const m=fixture();m.abi.requiredExports=m.abi.requiredExports.filter(x=>x!==name);assert.throws(()=>validateManifest(m),/workspace export/)}
 const pixel=fixture();pixel.build.featureSet='plr';assert.throws(()=>validateManifest(pixel),/feature mismatch/)
 const player=fixture();player.build.featureSet='wld';assert.throws(()=>validateManifest(player),/feature mismatch/)
})
