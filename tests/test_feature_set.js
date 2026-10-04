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

// The viewer WLD Web artifact exposes only APIs used by the current browser
// application. Node WLD keeps the broader compatibility/test surface.
const WLD_WEB_TRIMMED_EXPORTS = [
  "_terra_abi_version",
  "_terra_capabilities",
  "_tx_heap_used",
  "_tx_heap_peak",
  "_tx_bridge_heap_peak",
  "_tx_native_heap_peak",
  "_terra_world_open_from_buffer",
  "_terra_world_save_to_buffer",
  "_txw_apply_pixel_art",
];

test("compiled capabilities, manifest, identity, and exports agree on the feature set", async () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(ROOT, "build", "terra.manifest.json"), "utf8"));
  const featureSet = manifest.build.featureSet;
  const M = await TerraWorldWasm();
  const capabilities = JSON.parse(M.UTF8ToString(M._terra_capabilities()));
  const identity = JSON.parse(M.UTF8ToString(M._terra_build_info_json()));

  assert.deepEqual(capabilities.features, EXPECTED_FEATURES[featureSet]);
  assert.equal(identity.featureSet, featureSet);
  assert.equal(identity.viewerWebProfile, false, "Node is not a Web profile");
  assert.equal(manifest.build.viewerWebProfile, featureSet === "wld",
    "the compiled Web profile follows feature selection without another switch");

  const hasWld = featureSet !== "plr";
  const hasPlr = featureSet !== "wld";
  assert.equal(typeof M._terra_world_open_from_buffer, hasWld ? "function" : "undefined");
  assert.equal(typeof M._terra_plr_open_from_buffer, hasPlr ? "function" : "undefined");
  assert.equal(manifest.targets.node.exports.includes("_terra_world_open_from_buffer"), hasWld);
  assert.equal(manifest.targets.web.exports.includes("_terra_world_open_from_buffer"), featureSet === "all");
  assert.equal(manifest.targets.node.exports.includes("_terra_plr_open_from_buffer"), hasPlr);
  assert.equal(manifest.targets.web.exports.includes("_terra_plr_open_from_buffer"), hasPlr);

  if (featureSet === "wld") {
    for (const name of WLD_WEB_TRIMMED_EXPORTS) {
      assert.equal(manifest.targets.node.exports.includes(name), true, `${name} must remain available to Node WLD builds`);
      assert.equal(manifest.targets.web.exports.includes(name), false, `${name} must not be exported by the viewer WLD Web build`);
    }
    assert.equal(manifest.targets.web.exports.length, 39);
    for (const name of ["_txw_set_map_runtime", "_txw_use_builtin_map_runtime"]) {
      assert.equal(manifest.targets.web.exports.includes(name), true, `${name} must support shared local map resources`);
    }
    assert.equal(manifest.targets.node.exports.includes("_txw_add_pixel_art_chunk"), true);
    assert.equal(manifest.targets.node.exports.includes("_txw_add_pixel_art_chunks_bulk"), true);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunk"), false);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunks_bulk"), false);
    assert.equal(manifest.targets.web.exports.includes("_txw_add_pixel_art_chunks_bulk_fast"), true);
    assert.equal(manifest.targets.web.exports.includes("_txw_begin_pixel_art_indexed"), true);

    assert.equal(manifest.build.flags.web.includes("-sFILESYSTEM=0"), true,
      "WLD Web must remain filesystem-free");
    assert.equal(manifest.build.flags.web.includes("-sFILESYSTEM=1"), false,
      "WLD Web must not silently re-enable Emscripten filesystem glue");
    assert.equal(manifest.build.flags.node.includes("-sFILESYSTEM=1"), true,
      "Node WLD must retain Emscripten filesystem support");
    assert.equal(manifest.build.flags.node.includes("-sNODERAWFS=1"), true,
      "Node WLD must retain NODERAWFS compatibility");
  }
});
