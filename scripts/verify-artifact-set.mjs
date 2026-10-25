import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'
import { compiledWorkspaces, validateCompiledExports, validateManifest } from './generate-manifest.mjs'

// Acceptance runs against the downloaded release, not build-directory leftovers.
// Check every byte before executing either wrapper. An expected source commit is
// mandatory so a complete but stale release cannot certify newer source code.
export async function verifyArtifactSet({ directory, expectedSourceCommit }) {
  assert.match(expectedSourceCommit || '', /^[0-9a-f]{40}$/i, 'expected source commit is required')
  const root = path.resolve(directory)
  const manifest = validateManifest(JSON.parse(fs.readFileSync(path.join(root, 'terra.manifest.json'), 'utf8')))
  assert.equal(manifest.sourceCommit, expectedSourceCommit, 'release source commit differs from accepted source')
  const files = {}, names = new Set()
  for (const target of ['node', 'web']) {
    files[target] = {}
    for (const artifact of manifest.targets[target].artifacts) {
      const name = path.posix.basename(artifact.path)
      assert.equal(name, `terrax_world_wasm${target === 'web' ? '_web' : ''}.${artifact.role === 'wasm' ? 'wasm' : 'js'}`,
        `${target} ${artifact.role} has an unexpected release path`)
      assert.ok(!names.has(name), `duplicate artifact ${name}`)
      names.add(name)
      const file = path.join(root, name), bytes = fs.readFileSync(file)
      assert.equal(bytes.byteLength, artifact.bytes, `${name} byte count differs from manifest`)
      assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'), artifact.sha256,
        `${name} hash differs from manifest`)
      files[target][artifact.role] = { file, bytes }
    }
  }
  const require = createRequire(import.meta.url), modules = {}, identities = {}
  for (const target of ['node', 'web']) {
    const artifacts = files[target]
    delete require.cache[require.resolve(artifacts.wrapper.file)]
    const module = await require(artifacts.wrapper.file)({ wasmBinary: artifacts.wasm.bytes })
    const declared = manifest.targets[target]
    validateCompiledExports(module, declared.exports, target)
    const pointer = module._terra_build_info_json()
    assert.ok(Number.isSafeInteger(pointer) && pointer > 0 && pointer < module.HEAPU8.length,
      `${target} build identity pointer is invalid`)
    const end = module.HEAPU8.indexOf(0, pointer)
    assert.ok(end >= pointer, `${target} build identity is unterminated`)
    const identity = JSON.parse(Buffer.from(module.HEAPU8.subarray(pointer, end)).toString('utf8'))
    for (const [field, value] of Object.entries({
      sourceCommit: expectedSourceCommit, dirty: false, target,
      abiVersion: manifest.abi.version, compiler: manifest.build.compiler,
      featureSet: manifest.build.featureSet,
      viewerWebProfile: target === 'web' ? manifest.build.viewerWebProfile : false,
      initialMemory: declared.memory.initialBytes, maxMemory: declared.memory.maxBytes,
    })) assert.equal(identity[field], value, `${target} compiled ${field} differs from manifest`)
    // The compact all/WLD Web profile intentionally omits this general query;
    // its ABI version is in the compiled identity and each capability retains
    // its own required marker, checked below against the exact declaration.
    if (declared.exports.includes('_terra_abi_version')) {
      assert.equal(module._terra_abi_version(), manifest.abi.version, `${target} ABI marker differs`)
    }
    assert.equal(module.HEAPU8.byteLength, declared.memory.initialBytes, `${target} actual initial memory differs`)
    assert.deepEqual(identity.commonFlagsText.split(' @@ '), manifest.build.flags.common, `${target} common flags differ`)
    assert.deepEqual(identity.targetFlagsText.split(' @@ '), manifest.build.flags[target], `${target} target flags differ`)
    assert.deepEqual(identity.stream, manifest.abi.stream, `${target} stream identity differs`)
    if (manifest.abi.stream) assert.equal(module._terra_world_stream_abi_version(), manifest.abi.stream.version)
    const actualAbi = { version: identity.abiVersion,
      ...compiledWorkspaces(identity, module, declared.exports, target),
      ...(identity.stream ? { stream: identity.stream } : {}),
      requiredExports: manifest.abi.requiredExports, exportHash: manifest.abi.exportHash }
    assert.deepEqual(actualAbi, manifest.abi, `${target} compiled capabilities differ from manifest`)
    modules[target] = module
    identities[target] = identity
  }
  return { manifest, modules, identities }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv.length !== 4) throw new Error('Usage: node verify-artifact-set.mjs <artifact-directory> <expected-source-commit>')
  const { manifest } = await verifyArtifactSet({ directory: process.argv[2], expectedSourceCommit: process.argv[3] })
  console.log(`Verified Node/Web wrappers, Wasm, exports, ABI and memory: ${manifest.sourceCommit}`)
}
