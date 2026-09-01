import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

export const ABI_VERSION = 1
export const WEB_INITIAL_MEMORY = 64 * 1024 * 1024
export const WEB_MAXIMUM_MEMORY = 160 * 1024 * 1024
export const NODE_INITIAL_MEMORY = 128 * 1024 * 1024
export const NODE_MAXIMUM_MEMORY = 512 * 1024 * 1024
export const ID_EXPORTS = ['_terra_abi_version', '_terra_capabilities', '_terra_build_info_json']
export const FEATURE_SETS = Object.freeze(['all', 'wld', 'plr'])
export const EXPORTED_RUNTIME_METHODS_FLAG = "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','getValue','setValue','HEAPU8','HEAPU32','HEAP32','HEAPF32','HEAPF64','FS','stackAlloc','stackSave','stackRestore','wasmMemory']"
export const DEFAULT_COMMON_FLAGS = Object.freeze([
  '-O3',
  '-fno-exceptions',
  '-fno-rtti',
  '-sUSE_ZLIB=1',
  '-sALLOW_MEMORY_GROWTH=1',
  EXPORTED_RUNTIME_METHODS_FLAG,
  '-sMODULARIZE=1',
  '-sERROR_ON_UNDEFINED_SYMBOLS=1',
  '--no-entry',
])
export const DEFAULT_NODE_FLAGS = Object.freeze([
  '-sEXPORTED_FUNCTIONS=@exported_functions_node.json',
  '-sINITIAL_MEMORY=134217728',
  '-sMAXIMUM_MEMORY=536870912',
  "-sEXPORT_NAME='TerraWorldWasm'",
  '-sENVIRONMENT=node',
  '-sNODERAWFS=1',
  '-sFILESYSTEM=1',
])
export const DEFAULT_WEB_FLAGS = Object.freeze([
  '-sEXPORTED_FUNCTIONS=@exported_functions_web.json',
  '-sINITIAL_MEMORY=67108864',
  '-sMAXIMUM_MEMORY=167772160',
  "-sEXPORT_NAME='TerraWorldWasmWeb'",
  '-sENVIRONMENT=web,worker',
  '-sFILESYSTEM=1',
])

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
  const normalized = values.map((value) => String(value).trim()).filter(Boolean)
  if (normalized.length === 0) fail(`${label} is empty`)
  if (normalized.length !== values.length) fail(`${label} contains blank entries`)
  if (new Set(normalized).size !== normalized.length) fail(`${label} contains duplicates`)
  return normalized
}

function normalizeFeatureSet(value) {
  const normalized = String(value ?? '').trim().toLowerCase()
  if (!FEATURE_SETS.includes(normalized)) {
    fail(`feature set must be one of: ${FEATURE_SETS.join(', ')}`)
  }
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

function validateTarget(target, label, expectedMemory) {
  if (!target || typeof target !== 'object') fail(`${label} is missing`)
  if (target.memory?.initialBytes !== expectedMemory.initialBytes || target.memory?.maxBytes !== expectedMemory.maxBytes) {
    fail(`${label} memory is invalid`)
  }
  const exportsList = normalizeExports(target.exports, `${label} exports`)
  for (const name of ID_EXPORTS) {
    if (!exportsList.includes(name)) fail(`${label} exports are missing ${name}`)
  }
  if (!isSha256(target.exportHash)) fail(`${label} export hash is invalid`)
  if (target.exportHash !== exportHash(exportsList)) fail(`${label} export hash does not match the export set`)
  return {
    memory: {
      initialBytes: target.memory.initialBytes,
      maxBytes: target.memory.maxBytes,
    },
    exports: exportsList,
    exportHash: target.exportHash,
    artifacts: validateArtifacts(target.artifacts, `${label} artifacts`),
  }
}

function verifyWrapperIdentity(root, relativePath, label) {
  const wrapperSource = fs.readFileSync(path.resolve(root, relativePath), 'utf8')
  for (const name of ID_EXPORTS) {
    if (!wrapperSource.includes(name)) fail(`${label} is missing ${name}`)
  }
}

function targetInfo({ initialBytes, maxBytes, exportsList, artifacts }) {
  const normalizedExports = normalizeExports(exportsList)
  return {
    memory: { initialBytes, maxBytes },
    exports: normalizedExports,
    exportHash: exportHash(normalizedExports),
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
  const requiredExports = normalizeExports(abi.requiredExports, 'ABI export set')
  for (const name of ID_EXPORTS) {
    if (!requiredExports.includes(name)) fail(`ABI export set is missing ${name}`)
  }
  if (!isSha256(abi.exportHash)) fail('ABI export hash is invalid')
  if (abi.exportHash !== exportHash(requiredExports)) fail('ABI export hash does not match the export set')

  const build = manifest.build
  if (!build || typeof build.compiler !== 'string' || !build.compiler.trim()) {
    fail('compiler is required')
  }
  const featureSet = normalizeFeatureSet(build.featureSet ?? 'all')
  const buildFlags = normalizeBuildFlags(build.flags)

  const memory = manifest.memory
  if (memory?.initialBytes !== WEB_INITIAL_MEMORY || memory?.maxBytes !== WEB_MAXIMUM_MEMORY) {
    fail(`Web memory must be ${WEB_INITIAL_MEMORY}/${WEB_MAXIMUM_MEMORY} bytes`)
  }

  const nodeTarget = validateTarget(manifest.targets?.node, 'Node target', {
    initialBytes: NODE_INITIAL_MEMORY,
    maxBytes: NODE_MAXIMUM_MEMORY,
  })
  const webTarget = validateTarget(manifest.targets?.web, 'Web target', {
    initialBytes: WEB_INITIAL_MEMORY,
    maxBytes: WEB_MAXIMUM_MEMORY,
  })

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
      flags: buildFlags,
    },
    targets: {
      node: nodeTarget,
      web: webTarget,
    },
    artifacts: topLevelArtifacts,
  }
}

export function createManifest({
  root,
  sourceCommit,
  dirty,
  compiler,
  featureSet = 'all',
  buildFlags,
  flags,
  webWrapper,
  webWasm,
  webExports,
  nodeWrapper,
  nodeWasm,
  nodeExports,
}) {
  const normalizedBuildFlags = normalizeBuildFlags(buildFlags ?? {
    common: flags ?? DEFAULT_COMMON_FLAGS,
    node: DEFAULT_NODE_FLAGS,
    web: DEFAULT_WEB_FLAGS,
  })
  const normalizedFeatureSet = normalizeFeatureSet(featureSet)
  const normalizedWebExports = normalizeExports(webExports, 'Web target exports')
  const normalizedNodeExports = normalizeExports(nodeExports, 'Node target exports')
  const webArtifacts = [
    readArtifact(root, webWrapper, 'wrapper'),
    readArtifact(root, webWasm, 'wasm'),
  ]
  verifyWrapperIdentity(root, webWrapper, 'Web wrapper')
  const nodeArtifacts = [
    readArtifact(root, nodeWrapper, 'wrapper'),
    readArtifact(root, nodeWasm, 'wasm'),
  ]
  verifyWrapperIdentity(root, nodeWrapper, 'Node wrapper')
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
    build: {
      compiler,
      featureSet: normalizedFeatureSet,
      flags: normalizedBuildFlags,
    },
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

export function generateManifest({
  root,
  output,
  sourceCommit,
  dirty,
  compiler,
  featureSet,
  nodeExportsFile,
  webExportsFile,
  commonFlags,
  nodeFlags,
  webFlags,
} = {}) {
  const repositoryRoot = path.resolve(root ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..'))
  const manifest = createManifest({
    root: repositoryRoot,
    sourceCommit: sourceCommit ?? gitValue(repositoryRoot, ['rev-parse', 'HEAD'], '0'.repeat(40)),
    // Build outputs are regenerated by the build itself; publishability tracks source/config drift.
    dirty: dirty ?? Boolean(gitValue(repositoryRoot, ['status', '--porcelain', '--', '.', ':(exclude)build'], '')),
    compiler: compiler ?? (process.env.EMSCRIPTEN ? 'emscripten' : 'native'),
    featureSet: featureSet ?? 'all',
    buildFlags: {
      common: commonFlags?.length ? commonFlags : DEFAULT_COMMON_FLAGS,
      node: nodeFlags?.length ? nodeFlags : DEFAULT_NODE_FLAGS,
      web: webFlags?.length ? webFlags : DEFAULT_WEB_FLAGS,
    },
    webWrapper: 'build/terrax_world_wasm_web.js',
    webWasm: 'build/terrax_world_wasm_web.wasm',
    webExports: readExportFile(repositoryRoot, webExportsFile ?? 'exports.web.txt'),
    nodeWrapper: 'build/terrax_world_wasm.js',
    nodeWasm: 'build/terrax_world_wasm.wasm',
    nodeExports: readExportFile(repositoryRoot, nodeExportsFile ?? 'exports.txt'),
  })
  if (output) {
    fs.writeFileSync(path.resolve(repositoryRoot, output), `${JSON.stringify(manifest, null, 2)}\n`)
  }
  return manifest
}

function parseArgs(argv) {
  const args = {
    root: undefined,
    output: 'build/terra.manifest.json',
    sourceCommit: undefined,
    dirty: undefined,
    allowDirty: false,
    compiler: undefined,
    featureSet: 'all',
    nodeExportsFile: undefined,
    webExportsFile: undefined,
    commonFlags: [],
    nodeFlags: [],
    webFlags: [],
  }
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index]
    if (value === '--root') args.root = argv[++index]
    else if (value === '--output') args.output = argv[++index]
    else if (value === '--source-commit') args.sourceCommit = argv[++index]
    else if (value === '--dirty') {
      const dirtyValue = argv[++index]
      if (!['true', 'false'].includes(dirtyValue)) fail('--dirty must be true or false')
      args.dirty = dirtyValue === 'true'
    }
    else if (value === '--compiler') args.compiler = argv[++index]
    else if (value === '--feature-set') args.featureSet = argv[++index]
    else if (value === '--node-exports-file') args.nodeExportsFile = argv[++index]
    else if (value === '--web-exports-file') args.webExportsFile = argv[++index]
    else if (value === '--common-flag') args.commonFlags.push(argv[++index])
    else if (value === '--node-flag') args.nodeFlags.push(argv[++index])
    else if (value === '--web-flag') args.webFlags.push(argv[++index])
    else if (value === '--flags') args.commonFlags.push(argv[++index])
    else if (value === '--allow-dirty') args.allowDirty = true
    else fail(`unknown option ${value}`)
  }
  return args
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = parseArgs(process.argv.slice(2))
  const manifest = generateManifest(args)
  if (manifest.dirty) {
    if (args.allowDirty) {
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
