import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { validateManifest } from './generate-manifest.mjs'

export const WEB_ARTIFACT_LIMITS = Object.freeze({
  wrapperBytes: 128 * 1024,
  wasmBytes: 256 * 1024,
})

function fail(message) {
  throw new Error(`TerraWasm artifact size: ${message}`)
}

function findArtifact(artifacts, role) {
  const artifact = artifacts.find((entry) => entry.role === role)
  if (!artifact) fail(`missing ${role} artifact`)
  return artifact
}

export function verifyArtifactSizes({ root, manifest }) {
  const validatedManifest = validateManifest(manifest)
  const repositoryRoot = path.resolve(root)
  const wrapper = findArtifact(validatedManifest.artifacts, 'wrapper')
  const wasm = findArtifact(validatedManifest.artifacts, 'wasm')

  for (const artifact of [wrapper, wasm]) {
    const absolutePath = path.resolve(repositoryRoot, artifact.path)
    const stat = fs.statSync(absolutePath)
    if (stat.size !== artifact.bytes) {
      fail(`${artifact.role} byte count does not match disk (${artifact.bytes} != ${stat.size})`)
    }
  }

  if (wrapper.bytes > WEB_ARTIFACT_LIMITS.wrapperBytes) {
    fail(`wrapper size ${wrapper.bytes} exceeds ${WEB_ARTIFACT_LIMITS.wrapperBytes} bytes`)
  }
  if (wasm.bytes > WEB_ARTIFACT_LIMITS.wasmBytes) {
    fail(`wasm size ${wasm.bytes} exceeds ${WEB_ARTIFACT_LIMITS.wasmBytes} bytes`)
  }

  return {
    wrapperBytes: wrapper.bytes,
    wasmBytes: wasm.bytes,
    limits: WEB_ARTIFACT_LIMITS,
  }
}

function parseArgs(argv) {
  const args = {
    manifestPath: 'build/terra.manifest.json',
    root: undefined,
    allowDirty: false,
  }
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index]
    if (value === '--root') args.root = argv[++index]
    else if (value === '--allow-dirty') args.allowDirty = true
    else if (!args.manifestPath || args.manifestPath === 'build/terra.manifest.json') args.manifestPath = value
    else throw new Error(`Unknown option ${value}`)
  }
  return args
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = parseArgs(process.argv.slice(2))
  const manifestPath = path.resolve(args.manifestPath)
  const repositoryRoot = path.resolve(args.root ?? path.dirname(path.dirname(manifestPath)))
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'))
  if (args.allowDirty && manifest.dirty === true) {
    manifest.dirty = false
  }
  const result = verifyArtifactSizes({ root: repositoryRoot, manifest })
  console.log(JSON.stringify(result, null, 2))
}
