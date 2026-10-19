#!/usr/bin/env node
/** Reproduce viewport CPU/bridge work with a caller-owned WLD. No map or
 * extracted cell records are written into the report. Run the same input and
 * parameters against two clean builds and compare every resultSha256 first.
 * This is a Node-hosted Web Wasm benchmark, not a WeChat FPS measurement. */
import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { performance } from 'node:perf_hooks'

const args = process.argv.slice(2), sourcePath = args.shift()
if (!sourcePath || sourcePath.startsWith('--')) {
  throw new Error('Usage: node scripts/benchmark-circuit-viewports.mjs MAP.wld [--build-dir build] [--output report.json] [--repeat 20]')
}
const options = { 'build-dir': 'build', repeat: '20' }
while (args.length) {
  const key = args.shift()?.replace(/^--/, '')
  if (!['build-dir', 'output', 'repeat'].includes(key) || !args.length) throw new Error('Invalid benchmark option')
  options[key] = args.shift()
}
const repeat = Number(options.repeat)
assert.ok(Number.isInteger(repeat) && repeat >= 1 && repeat <= 100)
const build = path.resolve(options['build-dir'])
process.env.TCW_BUILD_DIR = build
const { makeDriver } = createRequire(import.meta.url)('../tests/helpers/circuit-world.js')
const bytes = fs.readFileSync(sourcePath), digest = value => crypto.createHash('sha256').update(value).digest('hex')
const d = await makeDriver(), M = d.M
const identity = JSON.parse(M.UTF8ToString(M._terra_build_info_json()))
const workUnits = 4096, maximumBytes = 128 * 1024 * 1024
const elapsedCpu = before => { const value = process.cpuUsage(before); return (value.user + value.system) / 1000 }

function pump(hash, metrics) {
  for (let steps = 0; steps < 10000000; steps++) {
    const event = d.event(false, workUnits); metrics.steps++
    if (event[1] === 4) return
    if (event[1] === 1) { metrics.sourceReads++; metrics.sourceBytes += event[4]; d.respond(event) }
    else if (event[1] === 2) d.respond(event)
    else if (event[1] === 3) {
      // Consume the borrowed buffer before ACK and obtain a fresh heap view
      // after every native call, including calls that may grow Wasm memory.
      const payload = M.HEAPU8.subarray(event[5], event[5] + event[4])
      hash.update(payload); metrics.resultBytes += event[4]
      assert.equal(M._terra_circuit_world_ack(d.circuitHandle), 0)
    }
  }
  throw new Error('Circuit benchmark did not complete')
}

const results = []
function series(name, request, count, dx = 0, dy = 0) {
  const hash = crypto.createHash('sha256'), times = [], metrics = { steps: 0, sourceReads: 0, sourceBytes: 0, resultBytes: 0 }
  const cpu = process.cpuUsage(), start = performance.now()
  for (let i = 0; i < count; i++) {
    const t = performance.now()
    assert.ok([0, 1].includes(d.start(1, { ...request, x: request.x + i * dx, y: request.y + i * dy })))
    pump(hash, metrics); times.push(performance.now() - t)
  }
  const wallMs = performance.now() - start, cpuMs = elapsedCpu(cpu), stats = d.stats()
  times.sort((a, b) => a - b)
  const ms = value => Number(value.toFixed(3))
  results.push({ name, request, count, dx, dy, wallMs: ms(wallMs), cpuMs: ms(cpuMs),
    medianMs: ms(times[Math.floor(count / 2)]), p95Ms: ms(times[Math.ceil(count * .95) - 1]), maxMs: ms(times.at(-1)),
    ...metrics, activeBytes: stats[16], peakBytes: stats[17], linearMemoryBytes: M.HEAPU8.byteLength,
    resultSha256: hash.digest('hex') })
}

let report
try {
  const cpu = process.cpuUsage(), started = performance.now()
  d.open(bytes, maximumBytes)
  const openAndCompileMs = performance.now() - started, openAndCompileCpuMs = elapsedCpu(cpu), stats = d.stats()
  const width = Math.min(stats[2], 64), height = Math.min(stats[3], 64)
  const x = Math.min(stats[2] - width, Math.max(0, stats[4] - Math.floor(width / 2)))
  const y = Math.min(stats[3] - height, Math.max(0, stats[5] - Math.floor(height / 2)))
  const request = { x, y, width, height, stride: 1, flags: 0 }
  series('first-foreground', request, 1)
  series('repeat-foreground', request, repeat)
  series('pan-x-foreground', request, Math.max(1, Math.min(repeat, stats[2] - x - width + 1)), 1)
  series('pan-y-foreground', request, Math.max(1, Math.min(repeat, stats[3] - y - height + 1)), 0, 1)
  series('repeat-walls', { ...request, flags: 2 }, repeat)
  series('pan-x-walls', { ...request, flags: 2 }, Math.max(1, Math.min(repeat, stats[2] - x - width + 1)), 1)
  series('overview-foreground', { x: 0, y: 0, width: stats[2], height: stats[3], stride: 64, flags: 0 }, 1)
  series('overview-walls', { x: 0, y: 0, width: stats[2], height: stats[3], stride: 64, flags: 2 }, 1)
  report = { schemaVersion: 1, runtime: { node: process.version, platform: process.platform, arch: process.arch },
    artifact: { sourceCommit: identity.sourceCommit, dirty: identity.dirty, featureSet: identity.featureSet,
      compiler: identity.compiler, wasmSha256: digest(fs.readFileSync(path.join(build, 'terrax_world_wasm_web.wasm'))) },
    source: { sha256: digest(bytes), sizeBytes: bytes.length, width: stats[2], height: stats[3] },
    parameters: { repeat, workUnits, maximumBytes },
    openAndCompile: { wallMs: Number(openAndCompileMs.toFixed(3)), cpuMs: Number(openAndCompileCpuMs.toFixed(3)),
      wireCells: stats[10], devices: stats[11], gates: stats[12], networks: stats[13], activeBytes: stats[16], peakBytes: stats[17] },
    results, note: 'Node-hosted Web Wasm with synchronous in-memory source reads and SHA-256 result consumption; no WeChat filesystem, rendering, IPC, device FPS, or process RSS claim.' }
} finally { d.dispose() }
const json = `${JSON.stringify(report, null, 2)}\n`
if (options.output) fs.writeFileSync(options.output, json)
else process.stdout.write(json)
