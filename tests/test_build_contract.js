"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const buildScript = fs.readFileSync(path.join(ROOT, "build.ps1"), "utf8");
const cmake = fs.readFileSync(path.join(ROOT, "CMakeLists.txt"), "utf8");

test("local builds deploy only when an explicit destination is supplied", () => {
  assert.doesNotMatch(
    buildScript,
    /C:\\Users\\depths\\Desktop\\Terra\\viewer-app/i,
    "the build script must not overwrite a hard-coded checkout",
  );
  assert.match(buildScript, /\[string\]\$DeployDir/);
  assert.match(buildScript, /if \(\$DeployDir\)/);
  assert.match(buildScript, /terra-wasm\.js/);
});

test("changing exports.txt automatically invalidates CMake configuration", () => {
  assert.match(cmake, /CMAKE_CONFIGURE_DEPENDS[\s\S]*NODE_EXPORTS_FILE/);
});

test("Node and Web targets use independent export lists", () => {
  assert.match(cmake, /exports\.web\.txt/);
  assert.match(cmake, /exported_functions_node\.json/);
  assert.match(cmake, /exported_functions_web\.json/);
});
