import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { verifyArtifactSet } from '../scripts/verify-artifact-set.mjs'

const root = fileURLToPath(new URL('..', import.meta.url))
const build = path.resolve(process.env.TCW_BUILD_DIR || path.join(root, 'build'))
const expectedSourceCommit = process.env.TCW_EXPECTED_COMMIT
  || execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim()
const original = JSON.parse(fs.readFileSync(path.join(build, 'terra.manifest.json'), 'utf8'))

for (const scenario of ['stale source', 'changed Web wrapper', 'changed Node Wasm', 'missing Node wrapper', 'changed manifest capability', 'omitted Node export', 'omitted Web export']) {
  test(`downloaded artifact gate rejects ${scenario}`, async () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'terra-artifact-gate-'))
    try {
      const manifest = structuredClone(original)
      for (const target of ['node', 'web']) for (const artifact of manifest.targets[target].artifacts) {
        const name = path.basename(artifact.path)
        fs.copyFileSync(path.join(build, name), path.join(directory, name))
      }
      let expected = expectedSourceCommit
      if (scenario === 'stale source') expected = '0'.repeat(40)
      if (scenario === 'changed Web wrapper' || scenario === 'changed Node Wasm') {
        const name = scenario === 'changed Web wrapper' ? 'terrax_world_wasm_web.js' : 'terrax_world_wasm.wasm'
        const bytes = fs.readFileSync(path.join(directory, name))
        bytes[bytes.length - 1] ^= 1
        fs.writeFileSync(path.join(directory, name), bytes)
      }
      if (scenario === 'missing Node wrapper') fs.unlinkSync(path.join(directory, 'terrax_world_wasm.js'))
      if (scenario === 'changed manifest capability') delete manifest.abi.worldWorkspace
      if (scenario.startsWith('omitted ')) {
        const target = scenario === 'omitted Node export' ? 'node' : 'web'
        const declared = manifest.targets[target]
        assert.ok(declared.exports.includes('_tx_malloc'))
        declared.exports = declared.exports.filter(name => name !== '_tx_malloc')
        declared.exportHash = crypto.createHash('sha256').update(`${declared.exports.join('\n')}\n`).digest('hex')
        if (target === 'web') {
          manifest.abi.requiredExports = declared.exports
          manifest.abi.exportHash = declared.exportHash
        }
      }
      fs.writeFileSync(path.join(directory, 'terra.manifest.json'), JSON.stringify(manifest))
      await assert.rejects(verifyArtifactSet({ directory, expectedSourceCommit: expected }), {
        'stale source': /source commit/,
        'changed Web wrapper': /hash differs/,
        'changed Node Wasm': /hash differs/,
        'missing Node wrapper': /ENOENT/,
        'changed manifest capability': /manifest|capabilities differ/,
        'omitted Node export': /undeclared exports: _tx_malloc/,
        'omitted Web export': /undeclared exports: _tx_malloc/,
      }[scenario])
    } finally { fs.rmSync(directory, { recursive: true, force: true }) }
  })
}

test('acceptance requires an explicit source identity before loading any artifact', async () => {
  await assert.rejects(verifyArtifactSet({ directory: build }), /expected source commit is required/)
})
