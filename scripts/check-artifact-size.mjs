import crypto from 'node:crypto'
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
  if (webWrapper.bytes > WEB_ARTIFACT_LIMITS.wrapperBytes) {
    fail(`wrapper size ${webWrapper.bytes} exceeds ${WEB_ARTIFACT_LIMITS.wrapperBytes} bytes`)
  }
  if (webWasm.bytes > WEB_ARTIFACT_LIMITS.wasmBytes) {
    fail(`wasm size ${webWasm.bytes} exceeds ${WEB_ARTIFACT_LIMITS.wasmBytes} bytes`)
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
