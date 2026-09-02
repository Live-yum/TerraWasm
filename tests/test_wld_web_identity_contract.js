"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const test = require("node:test");
const { pathToFileURL } = require("node:url");
const path = require("node:path");

const ROOT = path.resolve(__dirname, "..");
const sha256 = (value) => crypto.createHash("sha256").update(value).digest("hex");
const hashExports = (values) => sha256(Buffer.from(`${values.join("\n")}\n`));

function artifact(role, file) {
  return { role, path: file, bytes: 1, sha256: "a".repeat(64) };
}

test("WLD Web manifest requires build_info identity but not redundant ABI/capability exports", async () => {
  const { validateManifest } = await import(pathToFileURL(path.join(ROOT, "scripts", "generate-manifest.mjs")));
  const web = ["_terra_build_info_json", "_terra_world_open_begin"];
  const node = ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json", "_terra_world_open_from_buffer"];
  const webArtifacts = [artifact("wrapper", "build/web.js"), artifact("wasm", "build/web.wasm")];
  const manifest = {
    version: 1,
    artifactId: "terrax-world-web",
    sourceCommit: "0123456789abcdef0123456789abcdef01234567",
    dirty: false,
    abi: { version: 1, requiredExports: web, exportHash: hashExports(web) },
    memory: { initialBytes: 67108864, maxBytes: 167772160 },
    build: {
      compiler: "emscripten",
      featureSet: "wld",
      flags: { common: ["-O3"], node: ["-sENVIRONMENT=node"], web: ["-sENVIRONMENT=web,worker"] },
    },
    targets: {
      node: {
        memory: { initialBytes: 134217728, maxBytes: 536870912 },
        exports: node,
        exportHash: hashExports(node),
        artifacts: [artifact("wrapper", "build/node.js"), artifact("wasm", "build/node.wasm")],
      },
      web: {
        memory: { initialBytes: 67108864, maxBytes: 167772160 },
        exports: web,
        exportHash: hashExports(web),
        artifacts: webArtifacts,
      },
    },
    artifacts: webArtifacts,
  };

  assert.doesNotThrow(() => validateManifest(manifest));
  const missingBuildInfo = structuredClone(manifest);
  missingBuildInfo.abi.requiredExports = ["_terra_world_open_begin"];
  missingBuildInfo.abi.exportHash = hashExports(missingBuildInfo.abi.requiredExports);
  missingBuildInfo.targets.web.exports = missingBuildInfo.abi.requiredExports;
  missingBuildInfo.targets.web.exportHash = missingBuildInfo.abi.exportHash;
  assert.throws(() => validateManifest(missingBuildInfo), /missing _terra_build_info_json/);
});
