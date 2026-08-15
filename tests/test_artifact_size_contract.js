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

function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function exportHash(values) {
  return sha256(Buffer.from(`${values.join("\n")}\n`, "utf8"));
}

test("artifact size gate rejects manifest drift, digest drift, and oversized web artifacts", async () => {
  const { verifyArtifactSizes, WEB_ARTIFACT_LIMITS } = await loadSizeGate();
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), "terrawasm-size-"));
  try {
    const wrapperPath = path.join(temp, "wrapper.js");
    const wasmPath = path.join(temp, "module.wasm");
    const wrapperBytes = Buffer.from("x".repeat(1024));
    const wasmBytes = Buffer.alloc(2048, 7);
    const nodeWrapperBytes = Buffer.from("z".repeat(8));
    const nodeWasmBytes = Buffer.alloc(8, 5);
    fs.writeFileSync(wrapperPath, wrapperBytes);
    fs.writeFileSync(wasmPath, wasmBytes);
    fs.writeFileSync(path.join(temp, "node-wrapper.js"), nodeWrapperBytes);
    fs.writeFileSync(path.join(temp, "node-module.wasm"), nodeWasmBytes);

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
            { role: "wrapper", path: "node-wrapper.js", bytes: nodeWrapperBytes.byteLength, sha256: sha256(nodeWrapperBytes) },
            { role: "wasm", path: "node-module.wasm", bytes: nodeWasmBytes.byteLength, sha256: sha256(nodeWasmBytes) },
          ],
        },
        web: {
          memory: { initialBytes: 67108864, maxBytes: 167772160 },
          exports: ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"],
          exportHash: exportHash(["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json"]),
          artifacts: [
            { role: "wrapper", path: "wrapper.js", bytes: wrapperBytes.byteLength, sha256: sha256(wrapperBytes) },
            { role: "wasm", path: "module.wasm", bytes: wasmBytes.byteLength, sha256: sha256(wasmBytes) },
          ],
        },
      },
      artifacts: [
        { role: "wrapper", path: "wrapper.js", bytes: wrapperBytes.byteLength, sha256: sha256(wrapperBytes) },
        { role: "wasm", path: "module.wasm", bytes: wasmBytes.byteLength, sha256: sha256(wasmBytes) },
      ],
    };
    assert.doesNotThrow(() => verifyArtifactSizes({ root: temp, manifest }));

    fs.writeFileSync(wasmPath, Buffer.alloc(wasmBytes.byteLength, 8));
    assert.throws(() => verifyArtifactSizes({ root: temp, manifest }), /SHA-256/i);
    fs.writeFileSync(wasmPath, wasmBytes);

    fs.writeFileSync(path.join(temp, "node-module.wasm"), Buffer.alloc(nodeWasmBytes.byteLength, 6));
    assert.throws(() => verifyArtifactSizes({ root: temp, manifest }), /Node wasm SHA-256/i);
    fs.writeFileSync(path.join(temp, "node-module.wasm"), nodeWasmBytes);

    manifest.artifacts[0].bytes = 1023;
    manifest.targets.web.artifacts[0].bytes = 1023;
    assert.throws(() => verifyArtifactSizes({ root: temp, manifest }), /byte count/i);
    manifest.artifacts[0].bytes = wrapperBytes.byteLength;
    manifest.targets.web.artifacts[0].bytes = wrapperBytes.byteLength;

    const oversizedWrapper = Buffer.from("y".repeat(WEB_ARTIFACT_LIMITS.wrapperBytes + 1));
    fs.writeFileSync(wrapperPath, oversizedWrapper);
    manifest.artifacts[0].bytes = oversizedWrapper.byteLength;
    manifest.targets.web.artifacts[0].bytes = oversizedWrapper.byteLength;
    manifest.artifacts[0].sha256 = sha256(oversizedWrapper);
    manifest.targets.web.artifacts[0].sha256 = sha256(oversizedWrapper);
    assert.throws(() => verifyArtifactSizes({ root: temp, manifest }), /wrapper size/i);
  } finally {
    fs.rmSync(temp, { recursive: true, force: true });
  }
});
