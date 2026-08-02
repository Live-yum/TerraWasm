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
  assert.match(workflow, /wasm-release:\s*\n[\s\S]*build\.ps1[\s\S]*-Target all[\s\S]*-Test/);
  assert.match(workflow, /check-artifact-size\.mjs/);
  assert.equal((workflow.match(/actions\/checkout@v7/g) || []).length, 3);
  assert.equal((workflow.match(/actions\/setup-node@v7/g) || []).length, 1);
  assert.equal((workflow.match(/actions\/upload-artifact@v7/g) || []).length, 1);
  assert.match(workflow, /actions\/checkout@v7[\s\S]*fetch-depth: 0/);
  assert.doesNotMatch(workflow, /TerraX|viewer-app/);
});
