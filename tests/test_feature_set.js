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
    assert.equal(manifest.targets.node.exports.includes("_txw_add_pixel_art_chunk"), true);
    assert.equal(manifest.targets.node.exports.includes("_txw_add_pixel_art_chunks_bulk"), true);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunk"), false);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunks_bulk"), false);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunks_bulk_fast"), true);
  }
});