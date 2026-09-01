"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const WORKFLOW_PATH = path.join(ROOT, ".github", "workflows", "quality.yml");

test("CI runs the independent native, sanitizer/fuzz, and pinned Wasm release gates", () => {
  assert.equal(fs.existsSync(WORKFLOW_PATH), true, "the TerraWasm quality workflow must be checked in");
  const workflow = fs.readFileSync(WORKFLOW_PATH, "utf8");

  assert.match(workflow, /permissions:\s*\n\s+contents: read/);
  assert.match(workflow, /native:\s*\n[\s\S]*TERRAWASM_NATIVE_TESTS=ON[\s\S]*ctest/);
  assert.match(workflow, /sanitizer-and-fuzz:\s*\n[\s\S]*TERRAWASM_SANITIZERS=ON[\s\S]*terra_fuzz_smoke/);
  assert.match(workflow, /EMSCRIPTEN_VERSION: ["']?5\.0\.7["']?/);
  assert.match(workflow, /wasm-release:\s*\n[\s\S]*build\.ps1[\s\S]*-Target all[\s\S]*-Features all[\s\S]*-Test/);
  assert.match(workflow, /wasm-feature-matrix:[\s\S]*features:\s*\[wld, plr\][\s\S]*-Features \$env:TERRAWASM_FEATURE_SET/);
  assert.match(workflow, /check-artifact-size\.mjs/);
  assert.equal((workflow.match(/actions\/checkout@v7/g) || []).length, 4);
  assert.equal((workflow.match(/\blfs:\s*true/g) || []).length, 4,
    "every checkout that consumes LFS fixtures must hydrate them");
  assert.equal((workflow.match(/actions\/setup-node@v7/g) || []).length, 2);
  assert.equal((workflow.match(/actions\/upload-artifact@v7/g) || []).length, 2);
  assert.match(workflow, /name:\s*terrawasm-wld-\$\{\{ github\.sha \}\}/);
  assert.match(workflow, /if:\s*matrix\.features == ['"]wld['"]/);
  assert.match(workflow, /build\/terra\.manifest\.json[\s\S]*build\/terrax_world_wasm_web\.js[\s\S]*build\/terrax_world_wasm_web\.wasm/);
  assert.match(workflow, /actions\/checkout@v7[\s\S]*fetch-depth: 0/);
  assert.doesNotMatch(workflow, /TerraX|viewer-app/);
});
