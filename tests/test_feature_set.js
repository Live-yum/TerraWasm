"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const TerraWorldWasm = require(path.join(ROOT, "build", "terrax_world_wasm.js"));

const EXPECTED_FEATURES = {
  all: [
    "world-buffer-io",
    "json-sections",
    "preview-rgba",
    "thumbnail-png",
    "map-output",
    "pixel-art",
    "sha256",
    "plr-read-write",
  ],
  wld: [
    "world-buffer-io",
    "json-sections",
    "preview-rgba",
    "thumbnail-png",
    "map-output",
    "pixel-art",
    "sha256",
  ],
  plr: ["plr-read-write"],
};

const WLD_WEB_TRIMMED_EXPORTS = [
  "_terra_abi_version",
  "_terra_capabilities",
  "_tx_heap_used",
  "_tx_heap_peak",
  "_tx_bridge_heap_peak",
  "_tx_native_heap_peak",
];

test("compiled capabilities, manifest, identity, and exports agree on the feature set", async () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(ROOT, "build", "terra.manifest.json"), "utf8"));
  const featureSet = manifest.build.featureSet;
  const M = await TerraWorldWasm();
  const capabilities = JSON.parse(M.UTF8ToString(M._terra_capabilities()));
  const identity = JSON.parse(M.UTF8ToString(M._terra_build_info_json()));

  assert.deepEqual(capabilities.features, EXPECTED_FEATURES[featureSet]);
  assert.equal(identity.featureSet, featureSet);

  const hasWld = featureSet !== "plr";
  const hasPlr = featureSet !== "wld";
  assert.equal(typeof M._terra_world_open_from_buffer, hasWld ? "function" : "undefined");
  assert.equal(typeof M._terra_plr_open_from_buffer, hasPlr ? "function" : "undefined");
  assert.equal(manifest.targets.node.exports.includes("_terra_world_open_from_buffer"), hasWld);
  assert.equal(manifest.targets.web.exports.includes("_terra_world_open_from_buffer"), hasWld);
  assert.equal(manifest.targets.node.exports.includes("_terra_plr_open_from_buffer"), hasPlr);
  assert.equal(manifest.targets.web.exports.includes("_terra_plr_open_from_buffer"), hasPlr);

  if (featureSet === "wld") {
    for (const name of WLD_WEB_TRIMMED_EXPORTS) {
      assert.equal(manifest.targets.node.exports.includes(name), true, `${name} must remain available to Node WLD builds`);
      assert.equal(manifest.targets.web.exports.includes(name), false, `${name} must not be exported by the viewer WLD Web build`);
    }
    assert.equal(manifest.targets.web.exports.length, 30);
    assert.equal(manifest.targets.node.exports.includes("_txw_add_pixel_art_chunk"), true);
    assert.equal(manifest.targets.node.exports.includes("_txw_add_pixel_art_chunks_bulk"), true);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunk"), false);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunks_bulk"), false);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunks_bulk_fast"), true);
  }
});
