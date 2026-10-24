'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { makeDriver, makeCircuitWorld } = require('./helpers/circuit-world');

test('production Node/Web artifacts and world/player/pixel/circuit consumers agree', async () => {
  const build = path.resolve(process.env.TCW_BUILD_DIR || 'build');
  const { verifyArtifactSet } = await import('../scripts/verify-artifact-set.mjs');
  const expectedSourceCommit = process.env.TCW_EXPECTED_COMMIT
    || execFileSync('git', ['rev-parse', 'HEAD'], { cwd: path.join(__dirname, '..'), encoding: 'utf8' }).trim();
  const { manifest, modules } = await verifyArtifactSet({ directory: build, expectedSourceCommit });
  assert.equal(manifest.build.featureSet, 'all');
  for (const target of ['node', 'web']) {
    const d = await makeDriver(target, modules[target]);
    const M = d.M;
    const identity = JSON.parse(M.UTF8ToString(M._terra_build_info_json()));
    assert.equal(identity.featureSet, 'all');
    assert.equal(identity.sourceCommit, manifest.sourceCommit);
    assert.equal(M.HEAPU8.byteLength, manifest.targets[target].memory.initialBytes);
    let player = 0, pixel = 0, encoded = 0;
    const handle = M._tx_malloc(4);
    const input = Buffer.from(JSON.stringify(JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures/minimal-player.json')))) + '\0');
    const ptr = M._tx_malloc(input.length);
    assert.ok(handle && ptr);
    try {
      M.HEAPU8.set(input, ptr);
      assert.equal(M._terra_plr_open_json(ptr, handle), 0);
      player = M.HEAPU32[handle >>> 2];
      assert.ok(player);
      assert.equal(M._terra_plr_save_to_buffer(player, 0, 0, handle), 0);
      const size = M.HEAPU32[handle >>> 2];
      assert.ok(size > 0); encoded = M._tx_malloc(size); assert.ok(encoded);
      assert.equal(M._terra_plr_save_to_buffer(player, encoded, size, handle), 0);
      assert.equal(M._terra_plr_close(player), 0); player = 0;
      assert.equal(M._terra_plr_open_from_buffer(encoded, size, handle), 0);
      player = M.HEAPU32[handle >>> 2];
      assert.equal(M._terra_plr_close(player), 0); player = 0;
      assert.equal(M._terra_pixel_workspace_create(8, 8, 1048576, 65536, 256, handle), 0);
      pixel = M.HEAPU32[handle >>> 2];
      assert.equal(M._terra_pixel_workspace_tx_begin(pixel), 0);
      assert.equal(M._terra_pixel_workspace_palette_add(pixel, 0xff0000, handle), 0);
      const color = M.HEAPU32[handle >>> 2];
      M.HEAPU32.set([1, 1, color], ptr >>> 2);
      assert.equal(M._terra_pixel_workspace_cells(pixel, ptr, 1), 0);
      assert.equal(M._terra_pixel_workspace_tx_commit(pixel), 0);
      assert.equal(M._terra_pixel_workspace_undo(pixel), 0);
      assert.equal(M._terra_pixel_workspace_redo(pixel), 0);
      assert.equal(M._terra_pixel_workspace_close(pixel), 0); pixel = 0;
      d.open(makeCircuitWorld([]));
      assert.ok(d.worldHandle);
      d.command(1, { x: 0, y: 0 });
    } finally {
      if (player) M._terra_plr_close(player);
      if (pixel) M._terra_pixel_workspace_close(pixel);
      if (encoded) M._tx_free(encoded);
      M._tx_free(ptr); M._tx_free(handle); d.dispose();
    }
  }
});
