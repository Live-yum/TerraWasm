import test from 'node:test'
import assert from 'node:assert/strict'
import {createHash} from 'node:crypto'
import {readFileSync} from 'node:fs'
import {validateManifest, CIRCUIT_WORLD_EXPORTS} from '../scripts/generate-manifest.mjs'
const stream={version:2,inputLease:true,editPlan:true,pngColumnCursors:true,stampTiles:true,stampObjects:1}
const exports=['_terra_build_info_json',...['abi_version','acquire_input','commit_input','release_input','get_stats'].map(x=>'_terra_world_stream_'+x)]
const hash=x=>createHash('sha256').update(x).digest('hex')
function fixture(){
 const target=name=>({memory:{initialBytes:65536,maxBytes:65536},exports:[...exports],exportHash:hash(exports.join('\n')+'\n'),artifacts:[{role:'wrapper',path:`build/${name}.js`,bytes:1,sha256:'a'.repeat(64)},{role:'wasm',path:`build/${name}.wasm`,bytes:1,sha256:'b'.repeat(64)}]})
 const targets={node:target('node'),web:target('web')}
 return {version:1,artifactId:'terrax-world-web',sourceCommit:'a'.repeat(40),dirty:false,abi:{version:1,stream:{...stream},requiredExports:[...exports],exportHash:targets.web.exportHash},memory:targets.web.memory,build:{compiler:'test fixture only',featureSet:'wld',viewerWebProfile:true,flags:{common:['test'],node:['test'],web:['test']}},targets,artifacts:targets.web.artifacts}
}
test('source manifest validation accepts versioned stream contract and absent legacy contract',()=>{
 const m=fixture();assert.deepEqual(validateManifest(m).abi.stream,stream);delete m.abi.stream;assert.equal(validateManifest(m).abi.stream,undefined)
})
test('stream claims require exact capability contract and every export',()=>{
 for(const change of [s=>s.version=1,s=>s.inputLease=false,s=>s.editPlan=false,s=>s.pngColumnCursors=false,s=>s.stampTiles=false,s=>s.stampObjects=true,s=>s.extra=true]){const m=fixture();change(m.abi.stream);assert.throws(()=>validateManifest(m),/stream ABI/)}
 for(const name of exports.slice(1)){const m=fixture();m.abi.requiredExports=m.abi.requiredExports.filter(x=>x!==name);assert.throws(()=>validateManifest(m),/stream export/)}
})
test('every WLD export profile declares source ABI additions without modifying PLR',()=>{
 for(const file of ['exports.txt','exports.web.txt','exports.wld.txt','exports.wld.web.txt']){const names=readFileSync(new URL('../'+file,import.meta.url),'utf8').trim().split(/\r?\n/);assert.equal(new Set(names).size,names.length);for(const name of exports)assert.ok(names.includes(name),`${file}: ${name}`)}
 assert.ok(!readFileSync(new URL('../exports.plr.txt',import.meta.url),'utf8').includes('_terra_world_stream_'))
})

test('world circuit claims require the complete streaming, compact-state and atomic contract',()=>{
 const capability={version:1,fileBacked:true,streamingWld:true,streamingTwld:true,compiledNetworks:true,compactState:true,atomicCommands:true,wallLayer:true,fragments:true,fragmentObjects:1}
 function worldFixture(){
  const m=fixture();m.abi.circuitWorld={...capability};m.abi.requiredExports.push(...CIRCUIT_WORLD_EXPORTS)
  m.abi.exportHash=hash(m.abi.requiredExports.join('\n')+'\n')
  for(const target of Object.values(m.targets)){target.exports=[...m.abi.requiredExports];target.exportHash=m.abi.exportHash}
  return m
 }
 assert.deepEqual(validateManifest(worldFixture()).abi.circuitWorld,capability)
 for(const key of Object.keys(capability)){const m=worldFixture();m.abi.circuitWorld[key]=0;assert.throws(()=>validateManifest(m),/circuit world.*ABI/)}
 for(const missing of CIRCUIT_WORLD_EXPORTS){const m=worldFixture();m.abi.requiredExports=m.abi.requiredExports.filter(name=>name!==missing);assert.throws(()=>validateManifest(m),/circuit world.*export/)}
 const plr=worldFixture();plr.build.featureSet='plr';assert.throws(()=>validateManifest(plr),/feature mismatch/)
 for(const file of ['exports.txt','exports.web.txt','exports.wld.txt','exports.wld.web.txt']){
  const names=readFileSync(new URL('../'+file,import.meta.url),'utf8').trim().split(/\r?\n/)
  for(const name of CIRCUIT_WORLD_EXPORTS)assert.ok(names.includes(name),`${file}: ${name}`)
 }
 assert.ok(!readFileSync(new URL('../exports.plr.txt',import.meta.url),'utf8').includes('_terra_circuit_world_'))
})
