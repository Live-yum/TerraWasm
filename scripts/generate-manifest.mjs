import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const ABI_VERSION = 1
const WEB_INITIAL_MEMORY = 32 * 1024 * 1024
const WEB_MAXIMUM_MEMORY = 96 * 1024 * 1024
const NODE_INITIAL_MEMORY = 128 * 1024 * 1024
const NODE_MAXIMUM_MEMORY = 512 * 1024 * 1024
const ID_EXPORTS = ['_terra_abi_version', '_terra_capabilities', '_terra_build_info_json']

function fail(message) {
  throw new Error(`TerraWasm manifest: ${message}`)
}

function sha256(value) {
  return crypto.createHash('sha256').update(value).digest('hex')
}

function isSha256(value) {
  return typeof value === 'string' && /^[0-9a-f]{64}$/i.test(value)
}

function isCommit(value) {
  return typeof value === 'string' && /^[0-9a-f]{40}$/i.test(value)
}

function readArtifact(root, relativePath, role) {
  if (typeof relativePath !== 'string' || !relativePath || path.isAbsolute(relativePath)) {
    fail(`${role} artifact path must be repository-relative`)
  }
  const normalized = relativePath.replaceAll('\\', '/')
  if (normalized.split('/').includes('..')) fail(`${role} artifact path escapes the repository`)
  const absolutePath = path.resolve(root, relativePath)
  const bytes = fs.readFileSync(absolutePath)
  return {
    role,
    path: normalized,
    bytes: bytes.byteLength,
    sha256: sha256(bytes),
  }
}

function normalizeExports(exportsList) {
  const values = [...new Set((exportsList ?? []).map((value) => String(value).trim()).filter(Boolean))]
  return values
}

function exportHash(exportsList) {
  return sha256(Buffer.from(`${normalizeExports(exportsList).join('\n')}\n`, 'utf8'))
}

function targetInfo({ initialBytes, maxBytes, exportsList, artifacts }) {
  return {
    memory: { initialBytes, maxBytes },
    exports: normalizeExports(exportsList),
    exportHash: exportHash(exportsList),
    artifacts,
  }
}

export function validateManifest(manifest) {
  if (!manifest || typeof manifest !== 'object') fail('manifest must be an object')
  if (manifest.version !== 1) fail('version must be 1')
  if (manifest.artifactId !== 'terrax-world-web') fail('artifactId must be terrax-world-web')
  if (!isCommit(manifest.sourceCommit)) fail('source commit must be a 40-character commit hash')
  if (manifest.dirty !== false) fail('dirty build is not publishable')

  const abi = manifest.abi
  if (!abi || abi.version !== ABI_VERSION) fail('ABI version is invalid')
  const requiredExports = normalizeExports(abi.requiredExports)
  if (requiredExports.length === 0 || requiredExports.length !== (abi.requiredExports ?? []).length) {
    fail('ABI export set is empty or contains duplicates')
  }
  for (const name of ID_EXPORTS) {
    if (!requiredExports.includes(name)) fail(`ABI export set is missing ${name}`)
  }
  if (!isSha256(abi.exportHash)) fail('ABI export hash is invalid')
  if (abi.exportHash !== exportHash(requiredExports)) fail('ABI export hash does not match the export set')

  const memory = manifest.memory
  if (memory?.initialBytes !== WEB_INITIAL_MEMORY || memory?.maxBytes !== WEB_MAXIMUM_MEMORY) {
    fail(`Web memory must be ${WEB_INITIAL_MEMORY}/${WEB_MAXIMUM_MEMORY} bytes`)
  }
  if (manifest.targets?.web?.memory?.initialBytes !== WEB_INITIAL_MEMORY ||
      manifest.targets?.web?.memory?.maxBytes !== WEB_MAXIMUM_MEMORY) {
    fail('Web target memory does not match the top-level contract')
  }
  if (manifest.targets?.node?.memory?.initialBytes !== NODE_INITIAL_MEMORY ||
      manifest.targets?.node?.memory?.maxBytes !== NODE_MAXIMUM_MEMORY) {
    fail('Node target memory is invalid')
  }

  if (!manifest.build || typeof manifest.build.compiler !== 'string' || !Array.isArray(manifest.build.flags)) {
    fail('compiler and flags are required')
  }

  if (!Array.isArray(manifest.artifacts) || manifest.artifacts.length !== 2) {
    fail('exactly two Web artifacts are required')
  }
  const paths = new Set()
  for (const artifact of manifest.artifacts) {
    if (!artifact || !['wrapper', 'wasm'].includes(artifact.role)) fail('artifact role is invalid')
    if (paths.has(artifact.path)) fail(`duplicate artifact path: ${artifact.path}`)
    paths.add(artifact.path)
    if (!Number.isInteger(artifact.bytes) || artifact.bytes <= 0) fail(`${artifact.path}: byte count is invalid`)
    if (!isSha256(artifact.sha256)) fail(`${artifact.path}: SHA-256 is invalid`)
    if (artifact.role === 'wrapper' && !artifact.path.endsWith('.js')) fail('wrapper must be a JavaScript artifact')
    if (artifact.role === 'wasm' && !artifact.path.endsWith('.wasm')) fail('wasm must be a WebAssembly artifact')
  }
  return manifest
}

export function createManifest({
  root,
  sourceCommit,
  dirty,
  compiler,
  flags,
  webWrapper,
  webWasm,
  webExports,
  nodeWrapper,
  nodeWasm,
  nodeExports,
}) {
  const normalizedWebExports = normalizeExports(webExports)
  const normalizedNodeExports = normalizeExports(nodeExports)
  const webArtifacts = [
    readArtifact(root, webWrapper, 'wrapper'),
    readArtifact(root, webWasm, 'wasm'),
  ]
  const wrapperSource = fs.readFileSync(path.resolve(root, webWrapper), 'utf8')
  for (const name of ID_EXPORTS) {
    if (!wrapperSource.includes(name)) fail(`Web wrapper is missing ${name}`)
  }
  const nodeArtifacts = [
    readArtifact(root, nodeWrapper, 'wrapper'),
    readArtifact(root, nodeWasm, 'wasm'),
  ]
  return {
    version: 1,
    artifactId: 'terrax-world-web',
    sourceCommit,
    dirty,
    abi: {
      version: ABI_VERSION,
      requiredExports: normalizedWebExports,
      exportHash: exportHash(normalizedWebExports),
    },
    memory: { initialBytes: WEB_INITIAL_MEMORY, maxBytes: WEB_MAXIMUM_MEMORY },
    build: { compiler, flags: [...flags] },
    targets: {
      node: targetInfo({
        initialBytes: NODE_INITIAL_MEMORY,
        maxBytes: NODE_MAXIMUM_MEMORY,
        exportsList: normalizedNodeExports,
        artifacts: nodeArtifacts,
      }),
      web: targetInfo({
        initialBytes: WEB_INITIAL_MEMORY,
        maxBytes: WEB_MAXIMUM_MEMORY,
        exportsList: normalizedWebExports,
        artifacts: webArtifacts,
      }),
    },
    artifacts: webArtifacts,
  }
}

function gitValue(root, args, fallback) {
  try {
    return execFileSync('git', ['-C', root, ...args], { encoding: 'utf8' }).trim() || fallback
  } catch {
    return fallback
  }
}

function readExportFile(root, file) {
  return fs.readFileSync(path.resolve(root, file), 'utf8')
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith('#'))
}

export function generateManifest({ root, output, dirtyOverride, compiler, flags } = {}) {
  const repositoryRoot = path.resolve(root ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..'))
  const manifest = createManifest({
    root: repositoryRoot,
    sourceCommit: gitValue(repositoryRoot, ['rev-parse', 'HEAD'], '0'.repeat(40)),
    // Build outputs are checked into this repository and are regenerated by the build itself;
    // provenance dirty state therefore covers source/config changes, not generated build/ files.
    dirty: dirtyOverride ?? Boolean(gitValue(repositoryRoot, ['status', '--porcelain', '--', '.', ':(exclude)build'], '')),
    compiler: compiler ?? (process.env.EMSCRIPTEN ? 'emscripten' : 'native'),
    flags: flags ?? ['-O3', '-sALLOW_MEMORY_GROWTH=1'],
    webWrapper: 'build/terrax_world_wasm_web.js',
    webWasm: 'build/terrax_world_wasm_web.wasm',
    webExports: readExportFile(repositoryRoot, 'exports.web.txt'),
    nodeWrapper: 'build/terrax_world_wasm.js',
    nodeWasm: 'build/terrax_world_wasm.wasm',
    nodeExports: readExportFile(repositoryRoot, 'exports.txt'),
  })
  if (output) {
    fs.writeFileSync(path.resolve(repositoryRoot, output), `${JSON.stringify(manifest, null, 2)}\n`)
  }
  return manifest
}

function parseArgs(argv) {
  const args = { root: undefined, output: 'build/terra.manifest.json', dirtyOverride: undefined, compiler: undefined, flags: undefined }
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index]
    if (value === '--root') args.root = argv[++index]
    else if (value === '--output') args.output = argv[++index]
    else if (value === '--compiler') args.compiler = argv[++index]
    else if (value === '--flags') args.flags = argv[++index].split(',').map((flag) => flag.trim()).filter(Boolean)
    else if (value === '--allow-dirty') args.dirtyOverride = true
    else fail(`unknown option ${value}`)
  }
  return args
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = parseArgs(process.argv.slice(2))
  const manifest = generateManifest(args)
  if (manifest.dirty) {
    if (args.dirtyOverride === true) {
      console.warn('TerraWasm manifest generated from a dirty tree for local diagnostics; it is not publishable')
    } else {
      console.error('TerraWasm manifest generated from a dirty tree; publishable validation is intentionally refused')
      process.exitCode = 2
    }
  } else {
    validateManifest(manifest)
    console.log(`TerraWasm manifest generated: ${manifest.sourceCommit}`)
  }
}
