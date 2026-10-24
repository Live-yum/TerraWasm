import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { createRequire } from 'node:module'
import { instrumentBasicRuntimeHistory } from '../scripts/basic-runtime-history.mjs'

const require = createRequire(import.meta.url)
const factory = require('../build/terrax_world_wasm_web.js')
const wasmBinary = fs.readFileSync(new URL('../build/terrax_world_wasm_web.wasm', import.meta.url))
const page = 65536

async function fixture(extra = {}, repeatReceive = false) {
  let imports, memory, control
  const module = await factory({ wasmBinary,
    instantiateWasm(value, receive) {
      imports = value
      WebAssembly.instantiate(wasmBinary, value).then(result => {
        receive(result.instance, result.module)
        if (repeatReceive) receive(result.instance, result.module)
      })
      return {}
    },
    __terraBasicMemoryHandoff(value, lifecycle) { memory = value; control = lifecycle },
    ...extra,
  })
  // Import names are minified per feature profile. In the standalone PLR
  // artifact a.b is a filesystem import, not the heap resize hook. Resolve the
  // actual generated function from the real import object instead of invoking
  // an unrelated function and mistaking its errno for a growth result.
  const resizeImports = Object.values(imports).flatMap(namespace => Object.values(namespace))
    .filter(value => typeof value === 'function' && value.name === '_emscripten_resize_heap')
  assert.equal(resizeImports.length, 1, 'the real artifact must import exactly one reviewed heap resize hook')
  return { module, memory, control, resize: resizeImports[0] }
}

function countGrow(memory) {
  const original = memory.grow, calls = []
  memory.grow = function (pages) { calls.push(pages); return original.call(this, pages) }
  return calls
}

function blockRefresh(module) {
  let reads = 0
  const original = Object.getOwnPropertyDescriptor(module, 'HEAPU32')
  const opaque = new Proxy({}, { get() { reads++; throw null } })
  Object.defineProperty(module, 'HEAPU32', { configurable: true, set() { throw opaque } })
  return { restore() { Object.defineProperty(module, 'HEAPU32', original) }, reads: () => reads }
}

test('first publication hands off immutable private control then removes the only Module handoff', async () => {
  const { module, control } = await fixture()
  assert.equal(control.eligible(), true)
  assert.equal(Object.isFrozen(control), true)
  assert.deepEqual(Object.keys(control), ['eligible', 'enter', 'leave'])
  assert.equal(Object.getOwnPropertyDescriptor(module, '__terraBasicMemoryHandoff'), undefined)
  control.enter(); control.leave()
  assert.equal(control.eligible(), true)
})

test('one positive BASIC grow with failed refresh throws fixed poison without retry or throwable reads', async () => {
  const { module, memory, control, resize } = await fixture()
  const previous = memory.buffer.byteLength, calls = countGrow(memory)
  module.memoryGrowthLimit = () => previous + page
  const blocked = blockRefresh(module)
  control.enter()
  try {
    assert.throws(() => resize(previous + page), error => {
      assert.equal(Object.isFrozen(error), true)
      assert.deepEqual(Object.keys(error), ['name', 'code', 'message'])
      assert.equal(error.code, 'TRANSACTION_OUTCOME_UNKNOWN')
      return true
    })
  } finally { control.leave(); blocked.restore() }
  assert.deepEqual(calls, [1])
  assert.equal(memory.buffer.byteLength, previous + page)
  assert.equal(blocked.reads(), 0)
  assert.equal(control.eligible(), false)
  assert.throws(() => control.enter(), { code: 'TRANSACTION_OUTCOME_UNKNOWN' })
})

test('DEFAULT failed refresh preserves old grow-zero retries but permanently revokes eligibility', async () => {
  const { module, memory, control, resize } = await fixture()
  const previous = memory.buffer.byteLength, calls = countGrow(memory)
  module.memoryGrowthLimit = () => previous + page
  const blocked = blockRefresh(module)
  try { assert.equal(resize(previous + page), false) }
  finally { blocked.restore() }
  assert.deepEqual(calls, [1, 0, 0])
  assert.equal(blocked.reads(), 0)
  assert.equal(control.eligible(), false)
})

test('BASIC zero-page request poisons before native grow; native grow failures may retry without views', async () => {
  const zero = await fixture(), previous = zero.memory.buffer.byteLength
  const calls = countGrow(zero.memory)
  zero.module.memoryGrowthLimit = () => previous
  zero.control.enter()
  try { assert.throws(() => zero.resize(previous), { code: 'TRANSACTION_OUTCOME_UNKNOWN' }) }
  finally { zero.control.leave() }
  assert.deepEqual(calls, [])
  assert.equal(zero.control.eligible(), false)

  const failing = await fixture(), original = failing.module.HEAPU8
  let attempts = 0
  failing.memory.grow = () => { attempts++; throw null }
  failing.control.enter()
  try { assert.equal(failing.resize(failing.memory.buffer.byteLength + page), false) }
  finally { failing.control.leave() }
  assert.equal(attempts, 3)
  assert.equal(failing.module.HEAPU8, original)
  assert.equal(failing.control.eligible(), true)
})

test('BASIC nested resize revokes before the inner body even when generated maximum catches its failure', async () => {
  const { module, memory, control, resize } = await fixture()
  const previous = memory.buffer.byteLength, calls = countGrow(memory)
  let maximumCalls = 0
  module.memoryGrowthLimit = () => { maximumCalls++; return resize(previous + page) }
  control.enter()
  try { assert.equal(resize(previous + page), false) }
  finally { control.leave() }
  assert.equal(maximumCalls, 1)
  assert.deepEqual(calls, [])
  assert.equal(control.eligible(), false)
})

test('DEFAULT initialization growth after handoff and repeated receive cannot reset dirty history', async () => {
  const initialized = await fixture({ preRun(module) {
    const pointer = module._tx_malloc(20 * 1024 * 1024)
    module._tx_free(pointer)
  } })
  assert.equal(initialized.control.eligible(), false)
  const repeated = await fixture({}, true)
  assert.equal(repeated.control.eligible(), false)
  assert.equal(Object.getOwnPropertyDescriptor(repeated.module, '__terraBasicMemoryHandoff'), undefined)
})

test('failed initial view publication exposes no handoff or eligible certificate', async () => {
  let handoffs = 0, reads = 0, publications = 0, initialized = 0
  const opaque = new Proxy({}, { get() { reads++; throw null } })
  const options = { wasmBinary, __terraBasicMemoryHandoff() { handoffs++ },
    onRuntimeInitialized() { initialized++ } }
  Object.defineProperty(options, 'HEAPU32', { configurable: true, set() { publications++; throw opaque } })
  // assert.rejects returns the caught value through an async helper, which would
  // probe this hostile value's then property. Keep the throwable in this catch;
  // record only booleans, without returning or formatting the original value.
  let rejected = false, sameRejection = false
  try { await factory(options) }
  catch (error) { rejected = true; sameRejection = error === opaque }
  assert.equal(rejected, true)
  assert.equal(sameRejection, true)
  assert.equal(publications, 1)
  assert.equal(handoffs, 0)
  assert.equal(initialized, 0)
  assert.equal(reads, 0)
})

test('the generated wrapper cannot be instrumented twice or silently accept compiler template drift', () => {
  const wrapper = fs.readFileSync(new URL('../build/terrax_world_wasm_web.js', import.meta.url), 'utf8')
  assert.throws(() => instrumentBasicRuntimeHistory(wrapper), /BASIC loader template drift/)
})
