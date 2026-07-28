"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { pathToFileURL } = require("node:url");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");

function loadGenerator() {
  return import(pathToFileURL(path.join(ROOT, "scripts", "generate-manifest.mjs")));
}

function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

test("the artifact contract exposes the ABI identity exports", () => {
  const webExports = fs.readFileSync(path.join(ROOT, "exports.web.txt"), "utf8");
  const nodeExports = fs.readFileSync(path.join(ROOT, "exports.txt"), "utf8");
  for (const name of ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"]) {
    assert.match(webExports, new RegExp(`^${name}$`, "m"));
    assert.match(nodeExports, new RegExp(`^${name}$`, "m"));
  }
});

test("manifest validation rejects missing identity, memory, hash, and export data", async () => {
  const { validateManifest } = await loadGenerator();
  const base = {
    version: 1,
    artifactId: "terrax-world-web",
    sourceCommit: "0123456789abcdef0123456789abcdef01234567",
    dirty: false,
    abi: {
      version: 1,
      requiredExports: ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
      exportHash: sha256(Buffer.from("_terra_abi_version\n_terra_capabilities\n_terra_build_info_json\n")),
    },
    memory: { initialBytes: 33554432, maxBytes: 100663296 },
    build: { compiler: "test-compiler", flags: ["-O3"] },
    targets: {
      node: { memory: { initialBytes: 134217728, maxBytes: 536870912 } },
      web: { memory: { initialBytes: 33554432, maxBytes: 100663296 } },
    },
    artifacts: [
      { role: "wrapper", path: "terrax_world_wasm_web.js", bytes: 1, sha256: "b".repeat(64) },
      { role: "wasm", path: "terrax_world_wasm_web.wasm", bytes: 1, sha256: "c".repeat(64) },
    ],
  };

  for (const [label, mutate] of [
    ["ABI version", (manifest) => { manifest.abi.version = 2; }],
    ["source commit", (manifest) => { manifest.sourceCommit = "unknown"; }],
    ["dirty", (manifest) => { manifest.dirty = true; }],
    ["memory", (manifest) => { manifest.memory.maxBytes = 134217728; }],
    ["SHA", (manifest) => { manifest.artifacts[0].sha256 = "not-a-sha"; }],
    ["export set", (manifest) => { manifest.abi.requiredExports = []; }],
  ]) {
    const candidate = structuredClone(base);
    mutate(candidate);
    assert.throws(() => validateManifest(candidate), new RegExp(label, "i"));
  }

  assert.doesNotThrow(() => validateManifest(base));
});

test("manifest artifact hashes match the files on disk", async () => {
  const { createManifest, validateManifest } = await loadGenerator();
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), "terrawasm-manifest-"));
  try {
    const wrapperText = "wrapper _terra_abi_version _terra_capabilities _terra_build_info_json";
    fs.writeFileSync(path.join(temp, "wrapper.js"), wrapperText);
    fs.writeFileSync(path.join(temp, "module.wasm"), Buffer.from([0, 97, 115, 109]));
    const manifest = createManifest({
      root: temp,
      sourceCommit: "0123456789abcdef0123456789abcdef01234567",
      dirty: false,
      compiler: "test-compiler",
      flags: ["-O3"],
      webWrapper: "wrapper.js",
      webWasm: "module.wasm",
      webExports: ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
      nodeWrapper: "wrapper.js",
      nodeWasm: "module.wasm",
      nodeExports: ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
    });
    assert.equal(manifest.artifacts[0].bytes, Buffer.byteLength(wrapperText));
    assert.equal(manifest.artifacts[0].sha256, sha256(Buffer.from(wrapperText)));
    assert.equal(manifest.artifacts[1].sha256, sha256(Buffer.from([0, 97, 115, 109])));
    assert.equal(manifest.memory.maxBytes, 100663296);
    assert.equal(manifest.targets.node.memory.maxBytes, 536870912);
    assert.doesNotThrow(() => validateManifest(manifest));
  } finally {
    fs.rmSync(temp, { recursive: true, force: true });
  }
});
