import test from 'node:test'
import assert from 'node:assert/strict'
import vm from 'node:vm'
import fs from 'node:fs'
import { flattenBasicRuntimeFactory } from '../scripts/basic-runtime-history.mjs'

const prefix = 'var TerraWorldWasmWeb=(()=>{var _scriptName=globalThis.document?.currentScript?.src;return '
const body = 'async function(moduleArg={}){var Module=moduleArg;if(globalThis.WorkerGlobalScope){_scriptName=self.location.href}var moduleRtn={script:_scriptName,options:Module,receiver:this};await Promise.resolve();return moduleRtn}'
const exports = 'if(typeof exports==="object"&&typeof module==="object"){module.exports=TerraWorldWasmWeb;module.exports.default=TerraWorldWasmWeb}else if(typeof define==="function"&&define["amd"])define([],()=>TerraWorldWasmWeb);'
const source = prefix + body + '})();' + exports

function load(code, mode, worker) {
  let amd, scriptReads = 0
  const context = {
    document: { currentScript: { get src() { scriptReads++; return 'https://example.invalid/document.js' } } },
    self: { location: { href: 'https://example.invalid/worker.js' } },
  }
  if (worker) context.WorkerGlobalScope = function WorkerGlobalScope() {}
  if (mode === 'commonjs') { context.module = { exports: {} }; context.exports = context.module.exports }
  if (mode === 'amd') {
    context.define = (dependencies, factory) => { assert.equal(dependencies.length, 0); amd = factory() }
    context.define.amd = true
  }
  vm.createContext(context)
  new vm.Script(code).runInContext(context)
  const factory = mode === 'commonjs' ? context.module.exports : mode === 'amd' ? amd : context.TerraWorldWasmWeb
  return { factory, context, reads: () => scriptReads }
}

test('lexical-scope transform preserves the exact async body and export suffix', () => {
  const transformed = flattenBasicRuntimeFactory(source)
  assert.ok(transformed.includes(body))
  assert.ok(transformed.endsWith(exports))
  assert.ok(transformed.startsWith('var TerraWorldWasmWeb;{let _scriptName='))
  assert.ok(transformed.includes('TerraWorldWasmWeb=(0,async function('))
})

for (const mode of ['commonjs', 'amd', 'global']) for (const worker of [false, true]) {
  test(`factory scope and invocation contract: ${mode}/${worker ? 'Worker' : 'document'}`, async () => {
    const before = load(source, mode, worker)
    const after = load(flattenBasicRuntimeFactory(source), mode, worker)
    for (const value of [before, after]) {
      assert.equal(value.factory.name, '')
      assert.equal(value.factory.length, 0)
      assert.equal(value.factory.constructor.name, 'AsyncFunction')
      assert.equal(Object.hasOwn(value.context, '_scriptName'), false)
      assert.equal(value.reads(), 1)
      if (mode === 'commonjs') assert.equal(value.factory.default, value.factory)
      // Unrelated globals and a later document mutation cannot alter the cell
      // captured when this wrapper was evaluated. Workers intentionally update it.
      value.context._scriptName = 'https://example.invalid/unrelated.js'
      value.context.document.currentScript = { src: 'https://example.invalid/later-document.js' }
      const receiver = {}, options = {}
      const first = await value.factory.call(receiver, options)
      assert.equal(first.receiver, receiver)
      assert.equal(first.options, options)
      assert.equal(first.script, worker ? 'https://example.invalid/worker.js' : 'https://example.invalid/document.js')
      value.context.self.location.href = 'https://example.invalid/second-worker.js'
      const second = await value.factory()
      assert.equal(second.script, worker ? 'https://example.invalid/second-worker.js' : 'https://example.invalid/document.js')
      assert.equal(Object.keys(second.options).length, 0)
      assert.equal(value.reads(), 1)
      // Re-evaluation must not fail with a global lexical redeclaration, and an
      // earlier factory keeps its own cell after the global export is replaced.
      new vm.Script(flattenBasicRuntimeFactory(source)).runInContext(value.context)
      assert.equal((await value.factory()).script, second.script)
    }
  })
}

test('script URL getter exceptions escape before factory publication unchanged', () => {
  for (const code of [source, flattenBasicRuntimeFactory(source)]) {
    const failure = {}
    const context = vm.createContext({ document: { get currentScript() { throw failure } } })
    try { new vm.Script(code).runInContext(context); assert.fail('must throw') }
    catch (error) { assert.equal(error, failure) }
    assert.equal(context.TerraWorldWasmWeb, undefined)
  }
})

test('compiler drift, duplicate wrappers and repeated transformation fail closed', () => {
  for (const input of [
    source.replace('var _scriptName=', 'let _scriptName='),
    source.replace('moduleArg={}', 'moduleArg'),
    source.replace(';return moduleRtn}})();', ';return moduleRtn} })();'),
    source + source,
    flattenBasicRuntimeFactory(source),
  ]) assert.throws(() => flattenBasicRuntimeFactory(input), /BASIC loader template drift: factory lexical scope/)
})

test('compiled Web identity declares the exact producer transformation revision', () => {
  const cmake = fs.readFileSync(new URL('../CMakeLists.txt', import.meta.url), 'utf8')
  assert.ok(cmake.includes('list(APPEND WEB_IDENTITY_FLAGS "--wrapper-transform=basic-runtime-history-v3")'))
  assert.ok(!cmake.includes('--wrapper-transform=basic-runtime-history-v1'))
})
