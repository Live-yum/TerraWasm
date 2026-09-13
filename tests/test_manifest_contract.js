"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const { pathToFileURL } = require("node:url");

const ROOT = path.resolve(__dirname, "..");
const contract = import(pathToFileURL(path.join(ROOT, "scripts/generate-manifest.mjs")));
const manifest = JSON.parse(fs.readFileSync(path.join(ROOT, "build/terra.manifest.json"), "utf8"));

function copyBuild(root) {
  const build = path.join(root, "build");
  fs.mkdirSync(build);
  for (const target of ["node", "web"]) {
    for (const artifact of manifest.targets[target].artifacts) {
      fs.copyFileSync(path.join(ROOT, artifact.path), path.join(build, path.basename(artifact.path)));
    }
    fs.copyFileSync(path.join(ROOT, "build", `exported_functions_${target}.json`), path.join(build, `exported_functions_${target}.json`));
  }
  return build;
}

for (const [label, mutate] of [
  ["ABI version", m => { m.abi.version = 0; }],
  ["source commit", m => { m.sourceCommit = "unknown"; }],
  ["dirty identity", m => { m.dirty = true; }],
  ["compiler", m => { m.build.compiler = ""; }],
  ["feature set", m => { m.build.featureSet = ""; }],
  ["viewer profile", m => { m.build.viewerWebProfile = "true"; }],
  ["profile capability", m => { m.build.viewerWebProfile = true; m.build.featureSet = "plr"; }],
  ["build flags", m => { m.build.flags.common = []; }],
  ["ABI digest", m => { m.abi.exportHash = "0".repeat(64); }],
  ["blank export", m => { m.abi.requiredExports.push(""); }],
  ["duplicate export", m => { m.abi.requiredExports.push(m.abi.requiredExports[0]); }],
  ["Web memory drift", m => { m.memory.maxBytes -= 65536; }],
  ...["node", "web"].flatMap(target => [
    [`${target} memory alignment`, m => { m.targets[target].memory.maxBytes -= 1; }],
    [`${target} export hash`, m => { m.targets[target].exportHash = "0".repeat(64); }],
    [`${target} export drift`, m => { m.targets[target].exports.pop(); }],
    [`${target} artifact roles`, m => { m.targets[target].artifacts[1].role = "wrapper"; }],
    [`${target} artifact size`, m => { m.targets[target].artifacts[0].bytes = 0; }],
    [`${target} artifact hash`, m => { m.targets[target].artifacts[0].sha256 = "bad"; }],
  ]),
  ["Web artifact alias drift", m => { m.targets.web.artifacts[0].path = "different.js"; }],
  ...["/absolute.js", "C:\\absolute.js", "../outside.js", "..\\outside.js"].map(
    value => [value, m => { m.artifacts[0].path = value; }],
  ),
]) {
  test(`manifest rejects ${label}`, async () => {
    const { validateManifest } = await contract;
    const candidate = structuredClone(manifest);
    mutate(candidate);
    assert.throws(() => validateManifest(candidate), /manifest/i);
  });
}

test("real Node and Web modules reproduce the complete published manifest", async () => {
  const { generateManifest, validateManifest } = await contract;
  const actual = await generateManifest({ root: ROOT, sourceCommit: manifest.sourceCommit, dirty: manifest.dirty });
  assert.deepEqual(actual, manifest);
  assert.doesNotThrow(() => validateManifest(actual));
  await assert.rejects(generateManifest({ root: ROOT, sourceCommit: "0".repeat(40) }), /source commit/);
  await assert.rejects(generateManifest({ root: ROOT, dirty: !manifest.dirty }), /dirty state/);
});

test("schema validation does not prescribe the compiler's ABI version, feature set or memory defaults", async () => {
  const { validateManifest } = await contract;
  const candidate = structuredClone(manifest);
  candidate.abi.version += 1;
  candidate.build.featureSet = "additional-capability";
  candidate.build.viewerWebProfile = false;
  candidate.targets.node.memory.maxBytes += 65536;
  candidate.memory.maxBytes += 65536;
  candidate.targets.web.memory = candidate.memory;
  assert.doesNotThrow(() => validateManifest(candidate));
});

for (const scenario of ["stale exports", "mixed identity", "invalid wasm", "missing target", "duplicate target"]) {
  test(`generation rejects ${scenario} before writing metadata`, async () => {
    const { generateManifest } = await contract;
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "terra-manifest-"));
    try {
      const build = copyBuild(root);
      const nodeWasm = path.join(build, path.basename(manifest.targets.node.artifacts.find(a => a.role === "wasm").path));
      if (scenario === "stale exports") {
        fs.writeFileSync(path.join(build, "exported_functions_node.json"), JSON.stringify(["_missing_declared_export"]));
      } else if (scenario === "mixed identity" || scenario === "invalid wasm") {
        const bytes = fs.readFileSync(nodeWasm);
        if (scenario === "mixed identity") {
          const offset = bytes.indexOf(manifest.sourceCommit);
          assert.ok(offset >= 0);
          bytes.write("1".repeat(40), offset, "ascii");
        } else bytes[0] ^= 1;
        fs.writeFileSync(nodeWasm, bytes);
      } else if (scenario === "missing target") {
        fs.rmSync(nodeWasm);
      } else {
        fs.copyFileSync(nodeWasm, path.join(build, "duplicate.wasm"));
        fs.copyFileSync(nodeWasm.replace(/\.wasm$/, ".js"), path.join(build, "duplicate.js"));
      }
      await assert.rejects(generateManifest({ root, output: "build/result.json" }), {
        "stale exports": /missing_declared_export/,
        "mixed identity": /Node\/Web build identity sourceCommit mismatch/,
        "invalid wasm": /wasm|magic|WebAssembly/i,
        "missing target": /both Node and Web/,
        "duplicate target": /duplicate or invalid target/,
      }[scenario]);
      assert.equal(fs.existsSync(path.join(build, "result.json")), false);
    } finally {
      fs.rmSync(root, { recursive: true, force: true });
    }
  });
}
