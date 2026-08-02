"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { pathToFileURL } = require("node:url");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");

function loadSizeGate() {
  return import(pathToFileURL(path.join(ROOT, "scripts", "check-artifact-size.mjs")));
}

function exportHash(values) {
  return crypto.createHash("sha256").update(Buffer.from(`${values.join("\n")}\n`, "utf8")).digest("hex");
}

test("artifact size gate rejects manifest byte drift and oversized web artifacts", async () => {
  const { verifyArtifactSizes, WEB_ARTIFACT_LIMITS } = await loadSizeGate();
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), "terrawasm-size-"));
  try {
    const wrapperPath = path.join(temp, "wrapper.js");
    const wasmPath = path.join(temp, "module.wasm");
    fs.writeFileSync(wrapperPath, "x".repeat(1024));
    fs.writeFileSync(wasmPath, Buffer.alloc(2048, 7));

    const manifest = {
      version: 1,
      artifactId: "terrax-world-web",
      sourceCommit: "0123456789abcdef0123456789abcdef01234567",
      dirty: false,
      abi: {
        version: 1,
        requiredExports: ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
        exportHash: exportHash(["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"]),
      },
      memory: { initialBytes: 67108864, maxBytes: 167772160 },
      build: {
        compiler: "test-compiler",
        flags: {
          common: ["-O3", "-sUSE_ZLIB=1", "-sALLOW_MEMORY_GROWTH=1", "--no-entry"],
          node: ["-sINITIAL_MEMORY=134217728", "-sMAXIMUM_MEMORY=536870912", "-sENVIRONMENT=node", "-sFILESYSTEM=1"],
          web: ["-sINITIAL_MEMORY=67108864", "-sMAXIMUM_MEMORY=167772160", "-sENVIRONMENT=web,worker", "-sFILESYSTEM=1"],
        },
      },
      targets: {
        node: {
          memory: { initialBytes: 134217728, maxBytes: 536870912 },
          exports: ["_terra_world_open", "_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
          exportHash: exportHash(["_terra_world_open", "_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"]),
          artifacts: [
            { role: "wrapper", path: "node-wrapper.js", bytes: 8, sha256: "c".repeat(64) },
            { role: "wasm", path: "node-module.wasm", bytes: 8, sha256: "d".repeat(64) },
          ],
        },
        web: {
          memory: { initialBytes: 67108864, maxBytes: 167772160 },
          exports: ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
          exportHash: exportHash(["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"]),
          artifacts: [
            { role: "wrapper", path: "wrapper.js", bytes: 1024, sha256: "a".repeat(64) },
            { role: "wasm", path: "module.wasm", bytes: 2048, sha256: "b".repeat(64) },
          ],
        },
      },
      artifacts: [
        { role: "wrapper", path: "wrapper.js", bytes: 1024, sha256: "a".repeat(64) },
        { role: "wasm", path: "module.wasm", bytes: 2048, sha256: "b".repeat(64) },
      ],
    };
    fs.writeFileSync(path.join(temp, "node-wrapper.js"), "z".repeat(8));
    fs.writeFileSync(path.join(temp, "node-module.wasm"), Buffer.alloc(8, 5));

    assert.doesNotThrow(() => verifyArtifactSizes({ root: temp, manifest }));

    manifest.artifacts[0].bytes = 1023;
    manifest.targets.web.artifacts[0].bytes = 1023;
    assert.throws(() => verifyArtifactSizes({ root: temp, manifest }), /byte count/i);
    manifest.artifacts[0].bytes = 1024;
    manifest.targets.web.artifacts[0].bytes = 1024;

    fs.writeFileSync(wrapperPath, "y".repeat(WEB_ARTIFACT_LIMITS.wrapperBytes + 1));
    manifest.artifacts[0].bytes = WEB_ARTIFACT_LIMITS.wrapperBytes + 1;
    manifest.targets.web.artifacts[0].bytes = WEB_ARTIFACT_LIMITS.wrapperBytes + 1;
    assert.throws(() => verifyArtifactSizes({ root: temp, manifest }), /wrapper size/i);
  } finally {
    fs.rmSync(temp, { recursive: true, force: true });
  }
});
