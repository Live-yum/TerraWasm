"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const cmake = fs.readFileSync(path.join(ROOT, "CMakeLists.txt"), "utf8");
const build = fs.readFileSync(path.join(ROOT, "build.ps1"), "utf8");
const ops = fs.readFileSync(path.join(ROOT, "src", "terra_ops.c"), "utf8");
const render = fs.readFileSync(path.join(ROOT, "src", "terra_render.c"), "utf8");
const sourceApi = fs.readFileSync(path.join(ROOT, "src", "terra_api.c"), "utf8");
const update = fs.readFileSync(path.join(ROOT, "src", "terra_update.c"), "utf8");
const docs = fs.readFileSync(path.join(ROOT, "docs", "API.md"), "utf8");
const abi = fs.readFileSync(path.join(ROOT, "src", "terra_abi.c"), "utf8");
const manifest = fs.readFileSync(path.join(ROOT, "scripts", "generate-manifest.mjs"), "utf8");
const workflow = fs.readFileSync(path.join(ROOT, ".github", "workflows", "quality.yml"), "utf8");

function dispatcherClause(operation) {
  return `if (op_streq(op_name, "${operation}"))`;
}

function assertDispatcherRetired(operation) {
  assert.equal(
    ops.includes(dispatcherClause(operation)),
    false,
    `${operation} must be absent from the dispatcher`,
  );
}

function assertDispatcherRetained(operation) {
  assert.equal(
    ops.includes(dispatcherClause(operation)),
    true,
    `${operation} dispatcher clause must remain available`,
  );
}

test("viewer Web profile is WLD-only and target-local", () => {
  assert.match(cmake, /option\(TERRAWASM_VIEWER_WEB_PROFILE[\s\S]*OFF\)/);
  assert.match(cmake, /TERRAWASM_VIEWER_WEB_PROFILE AND NOT TERRAWASM_FEATURE_SET STREQUAL "wld"/);
  assert.match(cmake, /target_compile_definitions\(terrax_world_wasm_web PRIVATE TERRAWASM_VIEWER_WEB_PROFILE=1\)/);
  assert.doesNotMatch(cmake, /target_compile_definitions\(terrax_world_wasm PRIVATE TERRAWASM_VIEWER_WEB_PROFILE=1\)/);
  assert.match(build, /\[switch\]\$ViewerWebProfile/);
  assert.match(build, /-DTERRAWASM_VIEWER_WEB_PROFILE=\$ViewerWebProfileFlag/);
  assert.match(build, /--viewer-web-profile/);
  assert.match(manifest, /viewerWebProfile/);
  assert.match(abi, /TERRAX_VIEWER_WEB_PROFILE_JSON/);
});

test("viewer profile keeps retired operations out of every dispatcher", () => {
  for (const operation of [
    "render_preview_rgba",
    "mark_chest_items_preview",
    "mark_chest_items_map",
    "convert_world_biome",
    "set_visibility",
    "remove_all_wires",
    "unlock_bestiary",
  ]) {
    assertDispatcherRetired(operation);
  }
  assert.doesNotMatch(ops, /TERRAWASM_VIEWER_WEB_PROFILE/);

  for (const operation of [
    "render_preview_png",
    "render_thumbnail_png",
    "render_lit_map",
    "mark_tiles_and_chests_preview",
    "mark_tiles_and_chests_map",
    "batch_update_tiles",
    "header_patch",
    "replace_chests",
    "replace_bestiary",
  ]) {
    assertDispatcherRetained(operation);
  }
});

test("deprecated pixel mapping and legacy PNG compare implementation stay retired", () => {
  assert.doesNotMatch(ops, /apply_pixel_art_mapping/);
  assert.doesNotMatch(sourceApi, /apply_pixel_art_mapping/);
  assert.doesNotMatch(cmake, /TERRAX_ENABLE_LEGACY_RENDER_COMPARE/);
  for (const source of [sourceApi, update, render, docs]) {
    assert.doesNotMatch(
      source,
      /render_preview_rgba|mark_chest_items_preview|mark_chest_items_map|convert_world_biome|set_visibility|remove_all_wires|unlock_bestiary/,
    );
  }
  assert.match(abi, /viewerWebProfile.*TERRAX_VIEWER_WEB_PROFILE_JSON/);
  assert.match(manifest, /viewerWebProfile.*boolean/);
  assert.doesNotMatch(render, /TERRAX_ENABLE_LEGACY_RENDER_COMPARE|int32_t txw_render_preview_png\s*\(/);
});

test("historical compact TXCI artifacts stay absent", () => {
  for (const relative of [
    "data/terraria_compact.txci",
    "data/terraria_compact.manifest.json",
    "data/terraria_compact_b8.txci",
    "data/terraria_compact_b8.manifest.json",
  ]) {
    assert.equal(fs.existsSync(path.join(ROOT, relative)), false, `${relative} should stay retired`);
  }
});

test("quality workflow validates the viewer Web profile artifact", () => {
  assert.match(workflow, /Build and validate viewer Web profile/);
  assert.match(workflow, /-ViewerWebProfile/);
  assert.match(workflow, /check_viewer_web_profile_artifact\.js/);
  assert.doesNotMatch(workflow, /profileBytes\s+-ge\s+\$genericBytes/);
});
