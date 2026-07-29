"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const buildScript = fs.readFileSync(path.join(ROOT, "build.ps1"), "utf8");
const cmake = fs.readFileSync(path.join(ROOT, "CMakeLists.txt"), "utf8");
const manifestScript = fs.readFileSync(path.join(ROOT, "scripts", "generate-manifest.mjs"), "utf8");
const readme = fs.readFileSync(path.join(ROOT, "README.md"), "utf8");

test("local builds deploy only when an explicit destination is supplied", () => {
  assert.doesNotMatch(
    buildScript,
    /C:\\Users\\depths\\Desktop\\Terra\\viewer-app/i,
    "the build script must not overwrite a hard-coded checkout",
  );
  assert.match(buildScript, /\[string\]\$DeployDir/);
  assert.match(buildScript, /if \(\$DeployDir\)/);
  assert.match(buildScript, /terra-wasm\.js/);
  assert.match(buildScript, /\[string\]\$OptimizeFlag/);
  assert.match(buildScript, /\[switch\]\$EnableLto/);
  assert.match(buildScript, /check-artifact-size\.mjs/);
});

test("changing exports.txt automatically invalidates CMake configuration", () => {
  assert.match(cmake, /CMAKE_CONFIGURE_DEPENDS[\s\S]*NODE_EXPORTS_FILE/);
});

test("Node and Web targets use independent export lists", () => {
  assert.match(cmake, /exports\.web\.txt/);
  assert.match(cmake, /exported_functions_node\.json/);
  assert.match(cmake, /exported_functions_web\.json/);
  assert.match(cmake, /TERRAX_OPTIMIZE_FLAG/);
  assert.match(cmake, /TERRAX_ENABLE_LTO/);
  assert.match(cmake, /TERRAX_BUILD_COMMON_FLAGS_TEXT/);
  assert.match(cmake, /TERRAX_BUILD_TARGET_FLAGS_TEXT/);
  assert.match(cmake, /set\(COMMON_MANIFEST_FLAGS[\s\S]*TERRAX_OPTIMIZE_FLAG/);
  assert.doesNotMatch(cmake, /set\(COMMON_MANIFEST_FLAGS\s+\$\{COMMON_COMPILE_OPTIONS\}\s+\$\{COMMON_LINK_OPTIONS\}\)/);
  assert.match(cmake, /target_link_options\(terrax_world_wasm[\s\S]*TERRAX_OPTIMIZE_FLAG/);
  assert.match(cmake, /target_link_options\(terrax_world_wasm_web[\s\S]*TERRAX_OPTIMIZE_FLAG/);
  assert.match(buildScript, /--source-commit/);
  assert.match(buildScript, /--dirty/);
  assert.match(buildScript, /source changed during build/i);
  assert.match(manifestScript, /sourceCommit:\s*sourceCommit\s*\?\?/);
});

test("artifact paths are repository-relative on every host platform", () => {
  assert.match(manifestScript, /path\.posix\.isAbsolute/);
  assert.match(manifestScript, /path\.win32\.isAbsolute/);
});

test("the documented PowerShell size profile binds -Oz as a value", () => {
  assert.match(readme, /-OptimizeFlag\s+'-Oz'\s+-EnableLto/);
});
