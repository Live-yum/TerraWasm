import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { createServer } from 'node:http'
import { pathToFileURL } from 'node:url'

const root = process.cwd(), output = path.join(root, 'reports/hello-h5-screenshots')
fs.mkdirSync(output, { recursive: true })
assert.ok(path.isAbsolute(process.env.CIRCUIT_BROWSER_TOOLS || ''), 'CIRCUIT_BROWSER_TOOLS must be an absolute tooling directory')
const { chromium } = createRequire(path.join(process.env.CIRCUIT_BROWSER_TOOLS, 'package.json'))('playwright')
const { PNG } = createRequire(path.join(root, 'package.json'))('pngjs')
const moduleAt = file => import(pathToFileURL(path.join(root, file)).href)
const [{ serveBrowserStatic }, { buttonByLabel, clickToolbar }, { helloScreenWitness, readHelloPixels }, { HELLO_LAYOUT, HELLO_FRAMES }, { fitView }] = await Promise.all([
  moduleAt('scripts/browser-static-server.mjs'), moduleAt('scripts/circuit-browser-ui.mjs'),
  moduleAt('scripts/circuit-hello-browser.mjs'), moduleAt('features/circuit/domain/hello-example.mjs'),
  moduleAt('features/circuit/render/canvas.mjs'),
])
const report = { viewerCommit: process.env.VIEWER_COMMIT || null, startedAt: new Date().toISOString(), checks: [], passed: false }
const saveReport = () => fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify(report, null, 2) + '\n')
let browser, page
const deadline = setTimeout(() => {
  report.timeout = 'Screenshot run exceeded 175 seconds'; saveReport()
  void browser?.close().catch(() => {})
  setTimeout(() => process.exit(1), 3000)
}, 175000)
const server = createServer((req, res) => serveBrowserStatic(req, res, path.join(root, 'dist/build/h5')))
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
const origin = `http://127.0.0.1:${server.address().port}`
const safeUrl = url => { try { const parsed = new URL(url); return parsed.origin + parsed.pathname } catch { return String(url) } }

try {
  browser = await chromium.launch({ headless: true, timeout: 30000 })
  report.browser = { name: 'chromium', version: browser.version() }
  for (const viewport of [{ width: 1280, height: 900 }, { width: 390, height: 844 }]) {
    const check = { viewport, deviceScaleFactor: 1, passed: false, captures: [], console: [], pageErrors: [], failedRequests: [], wasmResponses: [] }
    report.checks.push(check); saveReport()
    try {
      page = await browser.newPage({ viewport, deviceScaleFactor: 1 })
      page.setDefaultTimeout(15000)
      page.on('console', message => check.console.push({ type: message.type(), text: message.text().slice(0, 2000) }))
      page.on('pageerror', error => check.pageErrors.push(error.message))
      page.on('requestfailed', request => check.failedRequests.push({ url: safeUrl(request.url()), reason: request.failure()?.errorText }))
      page.on('response', response => { if (/\.wasm(?:\?|$)/.test(response.url())) check.wasmResponses.push({ url: safeUrl(response.url()), status: response.status() }) })
      await page.goto(`${origin}/#/features/circuit/pages/circuit-page`, { waitUntil: 'domcontentloaded', timeout: 30000 })
      await page.locator('.cl-page:visible .circuit-surface').waitFor()
      await page.waitForFunction(() => document.querySelector('.cl-project')?.textContent.trim().startsWith('HELLO 像素屏'))
      await page.waitForFunction(() => {
        const canvas = document.querySelector('.cl-page .cl-stage .circuit-surface canvas')
        if (!canvas) return false
        const bounds = canvas.getBoundingClientRect(), ratio = Math.min(2, Math.max(1, devicePixelRatio))
        return bounds.width > 1 && bounds.height > 1 && canvas.width === Math.round(bounds.width * ratio) &&
          canvas.height === Math.round(bounds.height * ratio) && canvas.getContext('2d').getImageData(0, 0, 1, 1).data[3] > 0
      })
      await page.evaluate(() => document.fonts.ready)
      await buttonByLabel(page.locator('.cl-toolbar:visible'), '60 tick/s · 1×').waitFor()
      await buttonByLabel(page.locator('.cl-toolbar:visible'), '暂停').waitFor()

      // These source helpers provide only layout and original sprite colours.
      // The application is never given a fixture, engine call or replacement image.
      const { doc, witness } = helloScreenWitness()
      const bounds = await page.locator('.cl-page:visible .cl-stage .circuit-surface').boundingBox()
      assert.ok(bounds)
      const view = fitView(doc.world, bounds.width, bounds.height), screen = HELLO_LAYOUT.screen
      const points = Array.from({ length: screen.rows * screen.columns }, (_, i) => ({
        x: ((screen.x + i % screen.columns * screen.pitch) * 16 + witness.x - view.x) * view.zoom,
        y: ((screen.y + Math.floor(i / screen.columns) * screen.pitch) * 16 + witness.y - view.y) * view.zoom,
      }))
      assert.ok(points.every(point => point.x >= 0 && point.y >= 0 && point.x < bounds.width && point.y < bounds.height), 'all screen cells are in the actual initial viewport')
      const sampling = { points, dark: witness.dark, light: witness.light }
      const waitFrame = index => page.waitForFunction(readHelloPixels, { ...sampling, expected: HELLO_FRAMES[index].join('') }, { timeout: 12000 })
      const state = async () => ({
        label: await page.locator('.cl-clock-label:visible').textContent(),
        tick: Number((await page.locator('.cl-clock-label:visible').textContent()).match(/(\d+)\s*tick/)?.[1]),
        pixels: await page.evaluate(readHelloPixels, sampling),
      })
      // Verify the actual screenshot bytes too: a live letter can advance while
      // Playwright captures. Retrying takes a new browser screenshot; no pixels are edited.
      const screenshotPixels = bytes => {
        const png = PNG.sync.read(bytes), ratio = png.width / viewport.width
        assert.equal(png.width, viewport.width); assert.equal(png.height, viewport.height)
        return points.map(point => {
          const x = Math.floor((bounds.x + point.x) * ratio), y = Math.floor((bounds.y + point.y) * ratio)
          const rgba = [...png.data.subarray((y * png.width + x) * 4, (y * png.width + x) * 4 + 4)]
          const distance = colour => rgba.reduce((sum, v, i) => sum + (v - colour[i]) ** 2, 0)
          const a = distance(witness.dark), b = distance(witness.light)
          return Math.min(a, b) > 4 ? '?' : b < a ? '1' : '0'
        }).join('')
      }
      const capture = async (label, index) => {
        for (let attempt = 1; attempt <= 3; attempt++) {
          await waitFrame(index)
          const before = await state()
          const bytes = await page.screenshot({ fullPage: false, timeout: 10000 })
          const pixels = screenshotPixels(bytes), after = await state()
          check.captures.push({ label, attempt, before, after, screenshotPixels: pixels })
          if (pixels !== HELLO_FRAMES[index].join('')) continue
          const file = `${viewport.width}-${label}.png`
          fs.writeFileSync(path.join(output, file), bytes)
          check.captures.at(-1).file = file; saveReport(); return
        }
        throw new Error(`Could not capture the real ${label} letter within three live frames`)
      }
      check.entered = await state()
      await capture('autoplay-H', 0)
      await capture('autoplay-E', 1)
      check.autoplay = await state()
      assert.ok(check.autoplay.tick > check.entered.tick, 'HELLO must advance before any button click')

      let paused = false
      for (let attempt = 1; attempt <= 3; attempt++) {
        await waitFrame(0)
        await clickToolbar(page, '暂停')
        await page.waitForFunction(() => document.querySelector('.cl-clock-label')?.textContent.includes('已暂停'))
        if ((await state()).pixels === HELLO_FRAMES[0].join('')) { paused = true; break }
        await clickToolbar(page, '运行')
      }
      assert.ok(paused, 'pause H through the real toolbar within three attempts')
      await page.mouse.move(0, 0)
      await capture('paused-H', 0)
      check.paused = await state()
      check.visibleErrors = await page.locator('.cl-error:visible, .surface-error:visible').allTextContents()
      assert.deepEqual(check.visibleErrors, [])
      assert.deepEqual(check.pageErrors, [])
      check.passed = true
    } catch (error) {
      check.failure = String(error.stack || error)
      if (page) {
        await page.screenshot({ path: path.join(output, `${viewport.width}-failure.png`), fullPage: false, timeout: 5000 }).catch(() => {})
        check.visibleDom = await page.locator('body').innerText({ timeout: 3000 }).catch(() => '')
        check.visibleDom = check.visibleDom.slice(0, 20000)
      }
    } finally {
      fs.writeFileSync(path.join(output, `${viewport.width}-result.json`), JSON.stringify(check, null, 2) + '\n')
      await page?.close().catch(() => {}); page = null; saveReport()
    }
  }
  report.passed = report.checks.length === 2 && report.checks.every(check => check.passed)
  if (!report.passed) process.exitCode = 1
} catch (error) {
  report.failure = String(error.stack || error); process.exitCode = 1
} finally {
  clearTimeout(deadline)
  report.finishedAt = new Date().toISOString(); saveReport()
  await browser?.close().catch(() => {})
  await new Promise(resolve => server.close(resolve))
  console.log(JSON.stringify({ passed: report.passed, viewerCommit: report.viewerCommit, browser: report.browser, checks: report.checks.map(check => ({ viewport: check.viewport, passed: check.passed, files: check.captures.flatMap(capture => capture.file ? [capture.file] : []), failure: check.failure })) }, null, 2))
}
