"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const TerraWorldWasm = require(path.join(ROOT, "build", "terrax_world_wasm.js"));

function normalizedTargetFlags(value) {
  return value.split(" @@ ").map((flag) => flag
    .replace(/^SHELL:/, "")
    .replace(/@.*exported_functions_node\.json$/, "@exported_functions_node.json"));
}

test("the Node artifact reports the same source, flags, memory, and dirty identity as its manifest", async () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(ROOT, "build", "terra.manifest.json"), "utf8"));
  const M = await TerraWorldWasm();
  const identity = JSON.parse(M.UTF8ToString(M._terra_build_info_json()));

  assert.equal(identity.abiVersion, manifest.abi.version);
  assert.equal(identity.sourceCommit, manifest.sourceCommit);
  assert.equal(identity.dirty, manifest.dirty);
  assert.equal(typeof identity.dirty, "boolean");
  assert.equal(identity.target, "node");
  assert.equal(identity.initialMemory, manifest.targets.node.memory.initialBytes);
  assert.equal(identity.maxMemory, manifest.targets.node.memory.maxBytes);
  assert.deepEqual(identity.commonFlagsText.split(" @@ "), manifest.build.flags.common);
  assert.deepEqual(normalizedTargetFlags(identity.targetFlagsText), manifest.build.flags.node);
});
