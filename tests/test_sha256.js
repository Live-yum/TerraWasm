const assert = require('node:assert/strict')
const crypto = require('node:crypto')
const { test } = require('node:test')

const createModule = require('../build/terrax_world_wasm.js')

function expected(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex')
}

function hashWithWasm(module, bytes) {
  const input = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes)
  let inputPtr = 0
  let outputPtr = 0
  try {
    inputPtr = module._tx_malloc(Math.max(1, input.byteLength))
    outputPtr = module._tx_malloc(32)
    assert.notEqual(inputPtr, 0)
    assert.notEqual(outputPtr, 0)
    if (input.byteLength) module.HEAPU8.set(input, inputPtr)
    assert.equal(module._terra_sha256(inputPtr, input.byteLength, outputPtr), 0)
    return Buffer.from(module.HEAPU8.slice(outputPtr, outputPtr + 32)).toString('hex')
  } finally {
    if (outputPtr) module._tx_free(outputPtr)
    if (inputPtr) module._tx_free(inputPtr)
  }
}

function hashIncrementallyWithWasm(module, bytes, chunkSize = 64 * 1024) {
  const input = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes)
  let handlePtr = 0
  let chunkPtr = 0
  let outputPtr = 0
  let handle = 0
  let bridgeBytes = 0
  try {
    handlePtr = module._tx_malloc(4)
    chunkPtr = module._tx_malloc(chunkSize)
    outputPtr = module._tx_malloc(32)
    assert.notEqual(handlePtr, 0)
    assert.notEqual(chunkPtr, 0)
    assert.notEqual(outputPtr, 0)
    bridgeBytes = Number(module._tx_bridge_heap_used())
    assert.equal(module._terra_sha256_create(handlePtr), 0)
    handle = module.HEAPU32[handlePtr >>> 2] >>> 0
    assert.notEqual(handle, 0)
    for (let offset = 0; offset < input.byteLength; offset += chunkSize) {
      const chunk = input.subarray(offset, Math.min(offset + chunkSize, input.byteLength))
      module.HEAPU8.set(chunk, chunkPtr)
      assert.equal(module._terra_sha256_update(handle, chunkPtr, chunk.byteLength), 0)
    }
    assert.equal(module._terra_sha256_final(handle, outputPtr), 0)
    return {
      digest: Buffer.from(module.HEAPU8.slice(outputPtr, outputPtr + 32)).toString('hex'),
      bridgeBytes,
    }
  } finally {
    if (handle) module._terra_sha256_destroy(handle)
    if (outputPtr) module._tx_free(outputPtr)
    if (chunkPtr) module._tx_free(chunkPtr)
    if (handlePtr) module._tx_free(handlePtr)
  }
}

test('WASM SHA-256 matches standard vectors without retaining bridge memory', async () => {
  const module = await createModule()
  const large = crypto.randomBytes(1024 * 1024 + 13)
  const vectors = [
    new Uint8Array(0),
    Buffer.from('abc'),
    Buffer.from('TerraWasm immutable world revision'),
    large,
  ]
  const baseline = Number(module._tx_heap_used())
  for (const vector of vectors) {
    assert.equal(hashWithWasm(module, vector), expected(vector))
  }
  assert.equal(Number(module._tx_heap_used()), baseline)
})

test('incremental WASM SHA-256 bounds bridge memory and rejects stale handles', async () => {
  const module = await createModule()
  const bytes = crypto.randomBytes(4 * 1024 * 1024 + 13)
  const baseline = Number(module._tx_heap_used())
  const result = hashIncrementallyWithWasm(module, bytes)
  assert.equal(result.digest, expected(bytes))
  assert.equal(Number(module._tx_heap_used()), baseline)
  assert.equal(result.bridgeBytes - baseline, 64 * 1024 + 36)
  assert.equal(hashIncrementallyWithWasm(module, new Uint8Array(0)).digest, expected(new Uint8Array(0)))
  assert.equal(Number(module._tx_heap_used()), baseline)

  const handlePtr = module._tx_malloc(4)
  try {
    assert.equal(module._terra_sha256_create(handlePtr), 0)
    const staleHandle = module.HEAPU32[handlePtr >>> 2] >>> 0
    assert.equal(module._terra_sha256_destroy(staleHandle), 0)
    assert.notEqual(module._terra_sha256_update(staleHandle, 0, 0), 0)

    assert.equal(module._terra_sha256_create(handlePtr), 0)
    const nextHandle = module.HEAPU32[handlePtr >>> 2] >>> 0
    assert.notEqual(nextHandle, staleHandle)
    assert.notEqual(module._terra_sha256_destroy(staleHandle), 0)
    assert.equal(module._terra_sha256_destroy(nextHandle), 0)
  } finally {
    module._tx_free(handlePtr)
  }
  assert.equal(Number(module._tx_heap_used()), baseline)
})
