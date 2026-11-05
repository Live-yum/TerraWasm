import test from 'node:test'
import assert from 'node:assert/strict'
import vm from 'node:vm'
import fs from 'node:fs'
import { createHash } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { flattenBasicRuntimeFactory } from '../scripts/basic-runtime-history.mjs'
import { pruneNativeExportAliases } from '../scripts/prune-native-export-aliases.mjs'
import { parse, version } from '../scripts/vendor/acorn/acorn.mjs'

const raw = 'var TerraWorldWasmWeb=(()=>{var _scriptName=globalThis.document?.currentScript?.src;return async function(moduleArg={}){var Module=moduleArg;var _first,_second,used;function assignWasmExports(wasmExports){_first=Module["_first"]=wasmExports["a"];_second=Module["_second"]=wasmExports["b"];used=wasmExports["c"];}assignWasmExports(Module.exports);var moduleRtn={value:used,module:Module};return moduleRtn}})();'
const source = flattenBasicRuntimeFactory(raw)
const transform = text => pruneNativeExportAliases(flattenBasicRuntimeFactory(text))

test('only unread factory bindings and discarded outer assignments are removed', () => {
  const output = pruneNativeExportAliases(source)
  assert.ok(output.includes('var used;'))
  assert.ok(output.includes('Module["_first"]=wasmExports["a"];Module["_second"]=wasmExports["b"];used=wasmExports["c"];'))
  assert.ok(!output.includes('var _first'))
  assert.ok(!output.includes('_first=Module'))
  parse(output, { ecmaVersion: 2022 })
  assert.throws(() => pruneNativeExportAliases(output), /no provably unread aliases/)
})

test('native function values, descriptor flags, Proxy effects and assignment order are identical', async () => {
  const native = { a() {}, b() {}, c() {} }, traces = []
  for (const code of [source, pruneNativeExportAliases(source)]) {
    const trace = [], target = { exports: native }
    const module = new Proxy(target, {
      get(object, key, receiver) { trace.push(['get', key]); return Reflect.get(object, key, receiver) },
      set(object, key, value, receiver) { trace.push(['set', key, value]); return Reflect.set(object, key, value, receiver) },
      getOwnPropertyDescriptor(object, key) { trace.push(['descriptor', key]); return Reflect.getOwnPropertyDescriptor(object, key) },
      defineProperty(object, key, descriptor) { trace.push(['define', key, descriptor]); return Reflect.defineProperty(object, key, descriptor) },
    })
    const context = vm.createContext({})
    new vm.Script(code).runInContext(context)
    const result = await context.TerraWorldWasmWeb(module)
    assert.equal(result.module, module)
    assert.equal(result.value, native.c)
    assert.deepEqual(Object.keys(target), ['exports', '_first', '_second'])
    for (const [key, value] of [['_first', native.a], ['_second', native.b]]) {
      assert.deepEqual(Object.getOwnPropertyDescriptor(target, key), { value, writable: true, configurable: true, enumerable: true })
    }
    traces.push(trace)
  }
  assert.deepEqual(traces[0], traces[1])
})

test('a throwing export setter stops at the same assignment and propagates the same value', async () => {
  const failure = {}, traces = []
  for (const code of [source, pruneNativeExportAliases(source)]) {
    const trace = [], module = { exports: { a() {}, b() {}, c() {} } }
    Object.defineProperty(module, '_first', { set(value) { trace.push(value); throw failure } })
    const context = vm.createContext({})
    new vm.Script(code).runInContext(context)
    try { await context.TerraWorldWasmWeb(module); assert.fail('must reject') }
    catch (error) { assert.equal(error, failure) }
    assert.equal(Object.hasOwn(module, '_second'), false)
    traces.push(trace.length)
  }
  assert.deepEqual(traces, [1, 1])
})

for (const [name, addition] of [
  ['direct read', 'void _first;'],
  ['nested closure read', 'function later(){return _first}'],
  ['update', '_first++;'],
  ['reassignment', '_first=0;'],
  ['object shorthand', 'var saved={_first};'],
  ['destructuring read', 'var {_first:value}={};'],
  ['destructuring shadow', 'function later({_first}){}'],
  ['parameter shadow', 'function later(_first){}'],
  ['catch shadow', 'try{}catch(_first){}'],
  ['Unicode identifier read', 'void \\u005ffirst;'],
  ['direct eval', 'eval("_first");'],
  ['Function constructor', 'new Function("return _first");'],
  ['constructor indirection', '(()=>{}).constructor("return _first");'],
  ['with statement', 'with({}){}'],
]) test(`proof rejects ${name}`, () => {
  const altered = raw.replace('assignWasmExports(Module.exports);', addition + 'assignWasmExports(Module.exports);')
  assert.throws(() => transform(altered), /BASIC loader template drift/)
})

test('initialized, duplicate and non-factory bindings cannot be discarded', () => {
  assert.throws(() => transform(raw.replace('var _first,_second,used;', 'var _first=0,_second,used;')), /read or shadow/)
  assert.throws(() => transform(raw.replace('var _first,_second,used;', 'var _first,_first,_second,used;')), /duplicate declaration/)
  assert.throws(() => transform(raw.replace('var _first,_second,used;', 'var _second,used;').replace('function assignWasmExports(wasmExports){', 'function assignWasmExports(wasmExports){var _first;')), /export statement shape/)
})

test('export helper parameter and discarded-statement scope are checked', () => {
  assert.throws(() => transform(raw.replace('function assignWasmExports(wasmExports)', 'function assignWasmExports(wasmExports,Module)')), /export parameter/)
  assert.throws(() => transform(raw.replace('_first=Module', 'return _first=Module')), /export statement shape/)
  assert.throws(() => pruneNativeExportAliases(source.replace('TerraWorldWasmWeb=(0,', 'TerraWorldWasmWeb=(1,')), /factory scope/)
})

test('comments and strings are not mistaken for lexical identifier reads', () => {
  const altered = raw.replace('assignWasmExports(Module.exports);', '/* _first */ "_first"; assignWasmExports(Module.exports);')
  assert.ok(transform(altered).includes('/* _first */ "_first";'))
})

test('unsupported parser syntax fails before rewriting', () => {
  assert.throws(() => pruneNativeExportAliases(source + 'using x = foo();'))
})

test('the offline build parser is the exact vendored upstream version', () => {
  assert.equal(version, '8.15.0')
  const digest = file => createHash('sha256').update(fs.readFileSync(new URL(file, import.meta.url))).digest('hex')
  assert.equal(digest('../scripts/vendor/acorn/acorn.mjs'), 'b4c8c70200e72bae33cf1085e0ecb1e792c1b6924ed50cab817caf14f51bb249')
  assert.equal(digest('../scripts/vendor/acorn/LICENSE'), '76a876cf886ff9be2a8b5e2e86514fed06223c8c9f0c1e9ee9606e93841e00b7')
})

test('vendored upstream bytes survive Windows checkout conversion', () => {
  for (const file of ['acorn.mjs', 'LICENSE']) {
    const object = `HEAD:scripts/vendor/acorn/${file}`
    const options = { cwd: new URL('..', import.meta.url) }
    const original = execFileSync('git', ['show', object], options)
    const checkout = execFileSync('git', ['-c', 'core.autocrlf=true', 'cat-file', '--filters', object], options)
    assert.deepEqual(checkout, original, `${file} changed during checkout`)
  }
})
