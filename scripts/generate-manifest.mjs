import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'

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

function normalizeStringList(values, label) {
  if (!Array.isArray(values)) fail(`${label} must be an array`)
  if (values.some(value => typeof value !== 'string' || !value || value !== value.trim())) fail(`${label} contains blank or untrimmed entries`)
  const normalized = values
  if (normalized.length === 0) fail(`${label} is empty`)
  if (new Set(normalized).size !== normalized.length) fail(`${label} contains duplicates`)
  return normalized
}

function normalizeExports(exportsList, label = 'export set') {
  return normalizeStringList(exportsList, label)
}

function normalizeBuildFlags(buildFlags) {
  if (!buildFlags || typeof buildFlags !== 'object' || Array.isArray(buildFlags)) {
    fail('build flags must contain common/node/web arrays')
  }
  return {
    common: normalizeStringList(buildFlags.common, 'common build flags'),
    node: normalizeStringList(buildFlags.node, 'node build flags'),
    web: normalizeStringList(buildFlags.web, 'web build flags'),
  }
}

function exportHash(exportsList) {
  return sha256(Buffer.from(`${normalizeExports(exportsList).join('\n')}\n`, 'utf8'))
}

function isAbsoluteArtifactPath(value) {
  return path.posix.isAbsolute(value) || path.win32.isAbsolute(value)
}

function readArtifact(root, relativePath, role) {
  if (typeof relativePath !== 'string' || !relativePath) {
    fail(`${role} artifact path must be repository-relative`)
  }
  const normalized = relativePath.replaceAll('\\', '/')
  if (isAbsoluteArtifactPath(normalized)) fail(`${role} artifact path must be repository-relative`)
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

function validateArtifacts(artifacts, label) {
  if (!Array.isArray(artifacts) || artifacts.length !== 2) {
    fail(`${label} must contain exactly two artifacts`)
  }
  const paths = new Set()
  const roles = new Set()
  const normalized = artifacts.map((artifact) => {
    if (!artifact || !['wrapper', 'wasm'].includes(artifact.role)) {
      fail(`${label} contains an invalid artifact role`)
    }
    const normalizedPath = String(artifact.path || '').replaceAll('\\', '/')
    if (!normalizedPath || isAbsoluteArtifactPath(normalizedPath) || normalizedPath.split('/').includes('..')) {
      fail(`${label} contains an invalid artifact path`)
    }
    if (paths.has(normalizedPath)) fail(`${label} contains duplicate artifact paths`)
    paths.add(normalizedPath)
    if (roles.has(artifact.role)) fail(`${label} contains duplicate ${artifact.role} artifacts`)
    roles.add(artifact.role)
    if (!Number.isInteger(artifact.bytes) || artifact.bytes <= 0) {
      fail(`${label} contains an invalid byte count`)
    }
    if (!isSha256(artifact.sha256)) fail(`${label} contains an invalid SHA-256`)
    if (artifact.role === 'wrapper' && !normalizedPath.endsWith('.js')) {
      fail(`${label} wrapper must be a JavaScript artifact`)
    }
    if (artifact.role === 'wasm' && !normalizedPath.endsWith('.wasm')) {
      fail(`${label} wasm must be a WebAssembly artifact`)
    }
    return {
      role: artifact.role,
      path: normalizedPath,
      bytes: artifact.bytes,
      sha256: artifact.sha256,
    }
  })
  return normalized.sort((left, right) => left.role.localeCompare(right.role))
}

function artifactSignature(artifacts) {
  return artifacts.map((artifact) => `${artifact.role}|${artifact.path}|${artifact.bytes}|${artifact.sha256}`)
}

function validateMemory(memory, label) {
  if (!Number.isSafeInteger(memory?.initialBytes) || memory.initialBytes <= 0
    || !Number.isSafeInteger(memory?.maxBytes) || memory.maxBytes < memory.initialBytes
    || memory.initialBytes % 65536 || memory.maxBytes % 65536) fail(`${label} memory is invalid`)
  return memory
}

function validateTarget(target, label) {
  if (!target || typeof target !== 'object') fail(`${label} is missing`)
  const memory = validateMemory(target.memory, label)
  const exports = normalizeExports(target.exports, `${label} exports`)
  if (!isSha256(target.exportHash) || target.exportHash !== exportHash(exports)) {
    fail(`${label} export hash does not match the export set`)
  }
  return { memory, exports, exportHash: target.exportHash, artifacts: validateArtifacts(target.artifacts, `${label} artifacts`) }
}

export function validateManifest(manifest) {
  if (!manifest || typeof manifest !== 'object') fail('manifest must be an object')
  if (manifest.version !== 1) fail('version must be 1')
  if (manifest.artifactId !== 'terrax-world-web') fail('artifactId must be terrax-world-web')
  if (!isCommit(manifest.sourceCommit)) fail('source commit must be a 40-character commit hash')
  if (manifest.dirty !== false) fail('dirty build is not publishable')

  // Schema v1 predates feature selection; absent metadata meant the full build.
  const featureSet = manifest.build?.featureSet ?? 'all'
  if (typeof featureSet !== 'string' || !featureSet.trim()) fail('feature set is required')

  const abi = manifest.abi
  if (!abi || !Number.isSafeInteger(abi.version) || abi.version < 1) fail('ABI version is invalid')
  const requiredExports = normalizeExports(abi.requiredExports, 'ABI export set')
  if (!requiredExports.includes('_terra_build_info_json')) fail('ABI export set is missing _terra_build_info_json')
  if (!isSha256(abi.exportHash)) fail('ABI export hash is invalid')
  if (abi.exportHash !== exportHash(requiredExports)) fail('ABI export hash does not match the export set')

  const build = manifest.build
  if (!build || typeof build.compiler !== 'string' || !build.compiler.trim()) {
    fail('compiler is required')
  }
  const viewerWebProfile = build.viewerWebProfile === undefined ? false : build.viewerWebProfile
  if (typeof viewerWebProfile !== 'boolean') fail('viewer Web profile must be a boolean')
  if (viewerWebProfile && featureSet !== 'wld') fail('viewer Web profile feature set must be wld')
  const buildFlags = normalizeBuildFlags(build.flags)

  const nodeTarget = validateTarget(manifest.targets?.node, 'Node target')
  const webTarget = validateTarget(manifest.targets?.web, 'Web target')
  const memory = validateMemory(manifest.memory, 'Web')
  if (memory.initialBytes !== webTarget.memory.initialBytes || memory.maxBytes !== webTarget.memory.maxBytes) {
    fail('Web memory must match the Web target memory')
  }

  if (abi.exportHash !== webTarget.exportHash) fail('ABI export hash must match the Web target hash')
  if (requiredExports.join('\n') !== webTarget.exports.join('\n')) fail('ABI export set must match the Web target exports')

  const topLevelArtifacts = validateArtifacts(manifest.artifacts, 'top-level artifacts')
  if (artifactSignature(topLevelArtifacts).join('\n') !== artifactSignature(webTarget.artifacts).join('\n')) {
    fail('top-level artifacts must exactly match the Web target artifacts')
  }

  return {
    ...manifest,
    abi: {
      ...abi,
      requiredExports,
    },
    build: {
      compiler: build.compiler.trim(),
      featureSet,
      viewerWebProfile,
      flags: buildFlags,
    },
    targets: {
      node: nodeTarget,
      web: webTarget,
    },
    artifacts: topLevelArtifacts,
  }
}

// CMake owns export selection and linker flags. Read its existing export JSON
// and the actual compiled identities; do not reconstruct either in JavaScript.
export async function generateManifest({ root = process.cwd(), output, sourceCommit, dirty } = {}) {
  const repositoryRoot = path.resolve(root)
  const buildDir = path.join(repositoryRoot, 'build')
  const require = createRequire(import.meta.url)
  const targets = {}
  const identities = {}
  for (const file of fs.readdirSync(buildDir).filter(name => name.endsWith('.wasm')).sort()) {
    const wasmPath = path.join(buildDir, file)
    const wrapperPath = wasmPath.replace(/\.wasm$/, '.js')
    const wasmBinary = fs.readFileSync(wasmPath)
    delete require.cache[require.resolve(wrapperPath)]
    const module = await require(wrapperPath)({ wasmBinary })
    const ptr = module._terra_build_info_json()
    const end = module.HEAPU8.indexOf(0, ptr)
    if (!Number.isSafeInteger(ptr) || ptr <= 0 || end < ptr) fail(`${file}: build identity pointer is invalid`)
    const identity = JSON.parse(Buffer.from(module.HEAPU8.subarray(ptr, end)).toString('utf8'))
    const target = identity.target
    if (!['node', 'web'].includes(target) || targets[target]) fail(`${file}: duplicate or invalid target ${target}`)
    const exports = normalizeExports(JSON.parse(fs.readFileSync(path.join(buildDir, `exported_functions_${target}.json`), 'utf8')))
    for (const name of exports) {
      if (typeof module[name] !== 'function') fail(`${target} wrapper is missing ${name}`)
    }
    identities[target] = identity
    targets[target] = {
      memory: { initialBytes: identity.initialMemory, maxBytes: identity.maxMemory },
      exports,
      exportHash: exportHash(exports),
      artifacts: [
        readArtifact(repositoryRoot, path.relative(repositoryRoot, wrapperPath), 'wrapper'),
        readArtifact(repositoryRoot, path.relative(repositoryRoot, wasmPath), 'wasm'),
      ],
    }
  }
  if (!targets.node || !targets.web) fail('both Node and Web builds are required')
  const identity = identities.web
  for (const field of ['abiVersion', 'sourceCommit', 'dirty', 'compiler', 'featureSet', 'commonFlagsText']) {
    if (identities.node[field] !== identity[field]) fail(`Node/Web build identity ${field} mismatch`)
  }
  if (sourceCommit !== undefined && identity.sourceCommit !== sourceCommit) fail('compiled source commit does not match requested source commit')
  if (dirty !== undefined && identity.dirty !== dirty) fail('compiled dirty state does not match requested dirty state')
  const manifest = {
    version: 1,
    artifactId: 'terrax-world-web',
    sourceCommit: identity.sourceCommit,
    dirty: identity.dirty,
    abi: { version: identity.abiVersion, requiredExports: targets.web.exports, exportHash: targets.web.exportHash },
    memory: targets.web.memory,
    build: {
      compiler: identity.compiler,
      featureSet: identity.featureSet,
      viewerWebProfile: identity.viewerWebProfile,
      flags: {
        common: identity.commonFlagsText.split(' @@ '),
        node: identities.node.targetFlagsText.split(' @@ '),
        web: identity.targetFlagsText.split(' @@ '),
      },
    },
    targets,
    artifacts: targets.web.artifacts,
  }
  if (output) fs.writeFileSync(path.resolve(repositoryRoot, output), `${JSON.stringify(manifest, null, 2)}\n`)
  return manifest
}

function parseArgs(argv) {
  const args = { output: 'build/terra.manifest.json', allowDirty: false }
  for (let index = 0; index < argv.length; index += 1) {
    const option = argv[index]
    if (option === '--root') args.root = argv[++index]
    else if (option === '--output') args.output = argv[++index]
    else if (option === '--source-commit') args.sourceCommit = argv[++index]
    else if (option === '--dirty') {
      const value = argv[++index]
      if (!['true', 'false'].includes(value)) fail('--dirty must be true or false')
      args.dirty = value === 'true'
    } else if (option === '--allow-dirty') args.allowDirty = true
    else fail(`unknown option ${option}`)
  }
  return args
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = parseArgs(process.argv.slice(2))
  const manifest = await generateManifest(args)
  if (manifest.dirty && args.allowDirty) {
    console.warn('TerraWasm manifest generated from a dirty tree for local diagnostics; it is not publishable')
  } else {
    validateManifest(manifest)
    console.log(`TerraWasm manifest generated: ${manifest.sourceCommit}`)
  }
}
