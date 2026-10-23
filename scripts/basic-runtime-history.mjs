import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// Match the reviewed Emscripten loader exactly: compiler drift must fail the
// build rather than emit a wrapper that silently loses its growth certificate.
const patches = [
  [
    "factory bootstrap",
    "var Module=moduleArg;",
    "var Module=moduleArg;var terraBasicHistory=0,terraBasicActive=false,terraBasicResizing=false;var terraBasicFailure=Object.freeze({name:\"Error\",code:\"TRANSACTION_OUTCOME_UNKNOWN\",message:\"Circuit runtime growth boundary is unavailable. Do not retry.\"});var terraBasicControl=Object.freeze({eligible(){return terraBasicHistory===2},enter(){if(terraBasicHistory!==2||terraBasicActive){terraBasicHistory=-1;throw terraBasicFailure}terraBasicActive=true},leave(){terraBasicActive=false}});"
  ],
  [
    "first memory publication",
    "function receiveInstance(instance,module){wasmExports=instance.exports;assignWasmExports(wasmExports);updateMemoryViews();return wasmExports}",
    "function receiveInstance(instance,module){if(terraBasicHistory===0)terraBasicHistory=1;else terraBasicHistory=-1;wasmExports=instance.exports;assignWasmExports(wasmExports);updateMemoryViews();if(terraBasicHistory===1)terraBasicHistory=2;var terraBasicHandoff=Object.getOwnPropertyDescriptor(Module,\"__terraBasicMemoryHandoff\");if(terraBasicHandoff){if(!Reflect.deleteProperty(Module,\"__terraBasicMemoryHandoff\"))throw {name:\"Error\",code:\"TRANSACTION_OUTCOME_UNKNOWN\",message:\"Circuit memory handoff could not be removed\"};if(typeof terraBasicHandoff.value===\"function\")terraBasicHandoff.value(wasmMemory,terraBasicControl)}return wasmExports}"
  ],
  [
    "native growth and refresh",
    "var growMemory=size=>{var oldHeapSize=wasmMemory.buffer.byteLength;var pages=(size-oldHeapSize+65535)/65536|0;try{wasmMemory.grow(pages);updateMemoryViews();return 1}catch(e){}};",
    "var growMemory=size=>{var oldHeapSize=wasmMemory.buffer.byteLength;var pages=(size-oldHeapSize+65535)/65536|0;if(terraBasicActive){if(pages<=0){terraBasicHistory=-1;throw terraBasicFailure}try{wasmMemory.grow(pages)}catch{return 0}try{updateMemoryViews();return 1}catch{terraBasicHistory=-1;throw terraBasicFailure}}try{wasmMemory.grow(pages);updateMemoryViews();return 1}catch(e){}};"
  ],
  [
    "resize history and reentry",
    "var _emscripten_resize_heap=requestedSize=>{var oldSize=HEAPU8.length;requestedSize>>>=0;var maxHeapSize=getHeapMax();if(requestedSize>maxHeapSize){return false}for(var cutDown=1;cutDown<=4;cutDown*=2){var overGrownHeapSize=oldSize*(1+.2/cutDown);overGrownHeapSize=Math.min(overGrownHeapSize,requestedSize+100663296);var newSize=Math.min(maxHeapSize,alignMemory(Math.max(requestedSize,overGrownHeapSize),65536));var replacement=growMemory(newSize);if(replacement){return true}}return false};",
    "var _emscripten_resize_heap=requestedSize=>{if(terraBasicActive){if(terraBasicHistory!==2||terraBasicResizing){terraBasicHistory=-1;throw terraBasicFailure}terraBasicResizing=true}else terraBasicHistory=-1;try{var oldSize=HEAPU8.length;requestedSize>>>=0;var maxHeapSize=getHeapMax();if(requestedSize>maxHeapSize){return false}for(var cutDown=1;cutDown<=4;cutDown*=2){var overGrownHeapSize=oldSize*(1+.2/cutDown);overGrownHeapSize=Math.min(overGrownHeapSize,requestedSize+100663296);var newSize=Math.min(maxHeapSize,alignMemory(Math.max(requestedSize,overGrownHeapSize),65536));var replacement=growMemory(newSize);if(replacement){return true}}return false}finally{if(terraBasicActive)terraBasicResizing=false}};"
  ]
]

export function instrumentBasicRuntimeHistory(source) {
  for (const [label, before, after] of patches) {
    if (source.split(before).length !== 2) throw new Error(`BASIC loader template drift: ${label}`)
    source = source.replace(before, after)
  }
  return source
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv.length !== 3) throw new Error('Usage: node basic-runtime-history.mjs <generated-wrapper.js>')
  const file = process.argv[2]
  const source = instrumentBasicRuntimeHistory(fs.readFileSync(file, 'utf8'))
  fs.writeFileSync(file, source)
}
