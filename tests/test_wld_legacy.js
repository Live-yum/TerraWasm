"use strict";
const assert = require("node:assert/strict");
const path = require("node:path");
const test = require("node:test");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const { makeLegacyWorld } = require("./helpers/legacy-world");

function alloc(M, bytes) { const p=M._tx_malloc(bytes.length); assert.ok(p); M.HEAPU8.set(bytes,p); return p; }
test("release 87 WLD opens and preserves byte-exact legacy stream on save", async () => {
  const M=await TerraWorldWasm(), source=makeLegacyWorld(87), input=alloc(M,source), hp=M._tx_malloc(4);
  assert.equal(M._terra_world_open_from_buffer(input,source.length,hp),0); const h=M.HEAPU32[hp>>>2]; assert.ok(h);
  const sp=M._tx_malloc(4); assert.equal(M._terra_world_save_to_buffer(h,0,0,sp),0); const size=M.HEAPU32[sp>>>2], out=M._tx_malloc(size);
  assert.equal(M._terra_world_save_to_buffer(h,out,size,sp),0); assert.deepEqual(Buffer.from(M.HEAPU8.slice(out,out+size)),source);
  M._terra_world_close(h); M._tx_free(out); M._tx_free(sp); M._tx_free(hp); M._tx_free(input);
});
