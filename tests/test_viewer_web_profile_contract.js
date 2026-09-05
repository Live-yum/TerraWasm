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
const api = fs.readFileSync(path.join(ROOT, "docs", "API.md"), "utf8");
const abi = fs.readFileSync(path.join(ROOT, "src", "terra_abi.c"), "utf8");
const manifest = fs.readFileSync(path.join(ROOT, "scripts", "generate-manifest.mjs"), "utf8");
const workflow = fs.readFileSync(path.join(ROOT, ".github", "workflows", "quality.yml"), "utf8");

const PROFILE_GUARD = "#if !defined(TERRAWASM_VIEWER_WEB_PROFILE)";

function dispatcherPosition(operation) {
  const needle = `if (op_streq(op_name, "${operation}"))`;
  const position = ops.indexOf(needle);
  assert.notEqual(position, -1, `${operation} dispatcher clause must exist`);
  return position;
}

function assertProfileGuarded(operation) {
  const position = dispatcherPosition(operation);
  const guardStart = ops.lastIndexOf(PROFILE_GUARD, position);
  const previousEnd = ops.lastIndexOf("#endif", position);
  const nextEnd = ops.indexOf("#endif", position);
  assert.ok(guardStart >= 0, `${operation} must have a viewer-profile exclusion guard`);
  assert.ok(guardStart > previousEnd, `${operation} must be inside its nearest viewer-profile exclusion guard`);
  assert.ok(nextEnd > position, `${operation} viewer-profile exclusion guard must close after the dispatcher clause`);
}

function assertRetainedOutsideProfileGuard(operation) {
  const position = dispatcherPosition(operation);
  const guardStart = ops.lastIndexOf(PROFILE_GUARD, position);
  const guardEnd = ops.lastIndexOf("#endif", position);
  assert.ok(
    guardStart < 0 || guardEnd > guardStart,
    `${operation} must remain outside viewer-profile exclusion guards`,
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

test("viewer profile dispatcher excludes only compatibility operations", () => {
  for (const operation of [
    "render_preview_rgba",
    "mark_chest_items_preview",
    "mark_chest_items_map",
    "convert_world_biome",
    "set_visibility",
    "remove_all_wires",
    "unlock_bestiary",
  ]) {
    assertProfileGuarded(operation);
  }

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
    assertRetainedOutsideProfileGuard(operation);
  }
});

test("deprecated pixel mapping and legacy PNG compare implementation stay retired", () => {
  assert.doesNotMatch(ops, /apply_pixel_art_mapping/);
  assert.doesNotMatch(api, /apply_pixel_art_mapping/);
  assert.doesNotMatch(cmake, /TERRAX_ENABLE_LEGACY_RENDER_COMPARE/);
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

test("quality workflow performs an actual generic-vs-profile artifact comparison", () => {
  assert.match(workflow, /Build and compare viewer Web profile/);
  assert.match(workflow, /-ViewerWebProfile/);
  assert.match(workflow, /check_viewer_web_profile_artifact\.js/);
  assert.match(workflow, /profileBytes\s+-ge\s+\$genericBytes/);
});
