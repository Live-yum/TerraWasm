"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");
const TerraWorldWasm = require(path.join(ROOT, "build", "terrax_world_wasm.js"));

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
  assert.deepEqual(identity.targetFlagsText.split(" @@ "), manifest.build.flags.node);
  assert.doesNotMatch(identity.targetFlagsText, /SHELL:|[A-Za-z]:[\\/]/);
});

test("Wasm build identity does not embed the checkout path", () => {
  const checkoutPath = Buffer.from(ROOT.replaceAll("\\", "/"));
  for (const artifactName of ["terrax_world_wasm.wasm", "terrax_world_wasm_web.wasm"]) {
    const artifact = fs.readFileSync(path.join(ROOT, "build", artifactName));
    assert.equal(
      artifact.indexOf(checkoutPath),
      -1,
      `${artifactName} must not encode the absolute source or build directory`,
    );
  }
});
