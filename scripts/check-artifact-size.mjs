import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { validateManifest } from './generate-manifest.mjs'

export const WEB_ARTIFACT_LIMITS = Object.freeze({
  wrapperBytes: 128 * 1024,
  // The production WLD and standalone PLR Mini Program modules retain this cap.
  wasmBytes: 320 * 1024,
})
// The combined compatibility module includes every WLD and PLR API. With the
// pinned Emscripten 5.0.7 -Oz/LTO profile, master 8579be1 is 344389 bytes and
// circuit ABI v1 adds 3807 bytes (348196 total). 344 KiB is the smallest 8 KiB
// budget increment containing that measured artifact. WLD/PLR do not inherit it.
export const WEB_ARTIFACT_LIMITS_BY_FEATURE = Object.freeze({
  all: Object.freeze({ wrapperBytes: 128 * 1024, wasmBytes: 344 * 1024 }),
  wld: WEB_ARTIFACT_LIMITS,
  plr: WEB_ARTIFACT_LIMITS,
})

function fail(message) {
  throw new Error(`TerraWasm artifact size: ${message}`)
}

function findArtifact(artifacts, role) {
  const artifact = artifacts.find((entry) => entry.role === role)
  if (!artifact) fail(`missing ${role} artifact`)
  return artifact
}

function resolveArtifactPath(repositoryRoot, artifact) {
  const realRoot = fs.realpathSync(repositoryRoot)
  const absolutePath = fs.realpathSync(path.resolve(repositoryRoot, artifact.path))
  const relativePath = path.relative(realRoot, absolutePath)
  if (
    relativePath === '..'
    || relativePath.startsWith(`..${path.sep}`)
    || path.isAbsolute(relativePath)
  ) {
    fail(`${artifact.role} artifact resolves outside the repository root`)
  }
  return absolutePath
}

function sha256(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex')
}

export function verifyArtifactSizes({ root, manifest }) {
  const validatedManifest = validateManifest(manifest)
  const limits = WEB_ARTIFACT_LIMITS_BY_FEATURE[validatedManifest.build.featureSet] || WEB_ARTIFACT_LIMITS
  const repositoryRoot = path.resolve(root)
  const targets = [
    { label: 'Node', artifacts: validatedManifest.targets.node.artifacts },
    { label: 'Web', artifacts: validatedManifest.targets.web.artifacts },
  ]

  for (const target of targets) {
    const wrapper = findArtifact(target.artifacts, 'wrapper')
    const wasm = findArtifact(target.artifacts, 'wasm')
    for (const artifact of [wrapper, wasm]) {
      const absolutePath = resolveArtifactPath(repositoryRoot, artifact)
      const bytes = fs.readFileSync(absolutePath)
      if (bytes.byteLength !== artifact.bytes) {
        fail(`${target.label} ${artifact.role} byte count does not match disk (${artifact.bytes} != ${bytes.byteLength})`)
      }
      const digest = sha256(bytes)
      if (digest !== artifact.sha256.toLowerCase()) {
        fail(`${target.label} ${artifact.role} SHA-256 does not match disk (${artifact.sha256} != ${digest})`)
      }
    }
  }

  const webWrapper = findArtifact(validatedManifest.targets.web.artifacts, 'wrapper')
  const webWasm = findArtifact(validatedManifest.targets.web.artifacts, 'wasm')
  if (webWrapper.bytes > limits.wrapperBytes) {
    fail(`wrapper size ${webWrapper.bytes} exceeds ${limits.wrapperBytes} bytes`)
  }
  if (webWasm.bytes > limits.wasmBytes) {
    fail(`wasm size ${webWasm.bytes} exceeds ${limits.wasmBytes} bytes`)
  }

  return {
    wrapperBytes: webWrapper.bytes,
    wasmBytes: webWasm.bytes,
    node: {
      wrapperBytes: findArtifact(validatedManifest.targets.node.artifacts, 'wrapper').bytes,
      wasmBytes: findArtifact(validatedManifest.targets.node.artifacts, 'wasm').bytes,
    },
    web: {
      wrapperBytes: webWrapper.bytes,
      wasmBytes: webWasm.bytes,
    },
    limits,
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
