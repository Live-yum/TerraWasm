from pathlib import Path

path = Path('scripts/generate-manifest.mjs')
text = path.read_text(encoding='utf-8')


def replace_once(old: str, new: str, label: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected one match, got {count}')
    text = text.replace(old, new, 1)


replace_once(
    "export const ID_EXPORTS = ['_terra_abi_version', '_terra_capabilities', '_terra_build_info_json']\n",
    "export const ID_EXPORTS = ['_terra_abi_version', '_terra_capabilities', '_terra_build_info_json']\n"
    "export const WLD_WEB_ID_EXPORTS = ['_terra_build_info_json']\n",
    'identity constants',
)
replace_once(
    'function validateTarget(target, label, expectedMemory) {\n',
    'function validateTarget(target, label, expectedMemory, identityExports = ID_EXPORTS) {\n',
    'validateTarget signature',
)
replace_once(
    '  for (const name of ID_EXPORTS) {\n'
    '    if (!exportsList.includes(name)) fail(`${label} exports are missing ${name}`)\n'
    '  }\n',
    '  for (const name of identityExports) {\n'
    '    if (!exportsList.includes(name)) fail(`${label} exports are missing ${name}`)\n'
    '  }\n',
    'target identity loop',
)
replace_once(
    "function verifyWrapperIdentity(root, relativePath, label) {\n"
    "  const wrapperSource = fs.readFileSync(path.resolve(root, relativePath), 'utf8')\n"
    "  for (const name of ID_EXPORTS) {\n",
    "function verifyWrapperIdentity(root, relativePath, label, identityExports = ID_EXPORTS) {\n"
    "  const wrapperSource = fs.readFileSync(path.resolve(root, relativePath), 'utf8')\n"
    "  for (const name of identityExports) {\n",
    'wrapper identity signature',
)
replace_once(
    '  const abi = manifest.abi\n',
    "  const featureSet = normalizeFeatureSet(manifest.build?.featureSet ?? 'all')\n"
    "  const webIdentityExports = featureSet === 'wld' ? WLD_WEB_ID_EXPORTS : ID_EXPORTS\n\n"
    '  const abi = manifest.abi\n',
    'manifest feature identity setup',
)
replace_once(
    '  for (const name of ID_EXPORTS) {\n'
    '    if (!requiredExports.includes(name)) fail(`ABI export set is missing ${name}`)\n'
    '  }\n',
    '  for (const name of webIdentityExports) {\n'
    '    if (!requiredExports.includes(name)) fail(`ABI export set is missing ${name}`)\n'
    '  }\n',
    'ABI identity loop',
)
replace_once(
    "  const featureSet = normalizeFeatureSet(build.featureSet ?? 'all')\n"
    '  const buildFlags = normalizeBuildFlags(build.flags)\n',
    '  const buildFlags = normalizeBuildFlags(build.flags)\n',
    'duplicate feature set',
)
replace_once(
    "  const webTarget = validateTarget(manifest.targets?.web, 'Web target', {\n"
    '    initialBytes: WEB_INITIAL_MEMORY,\n'
    '    maxBytes: WEB_MAXIMUM_MEMORY,\n'
    '  })\n',
    "  const webTarget = validateTarget(manifest.targets?.web, 'Web target', {\n"
    '    initialBytes: WEB_INITIAL_MEMORY,\n'
    '    maxBytes: WEB_MAXIMUM_MEMORY,\n'
    '  }, webIdentityExports)\n',
    'web target identity validation',
)
replace_once(
    '  const normalizedFeatureSet = normalizeFeatureSet(featureSet)\n'
    "  const normalizedWebExports = normalizeExports(webExports, 'Web target exports')\n",
    '  const normalizedFeatureSet = normalizeFeatureSet(featureSet)\n'
    "  const webIdentityExports = normalizedFeatureSet === 'wld' ? WLD_WEB_ID_EXPORTS : ID_EXPORTS\n"
    "  const normalizedWebExports = normalizeExports(webExports, 'Web target exports')\n",
    'create manifest identity setup',
)
replace_once(
    "  verifyWrapperIdentity(root, webWrapper, 'Web wrapper')\n",
    "  verifyWrapperIdentity(root, webWrapper, 'Web wrapper', webIdentityExports)\n",
    'web wrapper validation',
)

path.write_text(text, encoding='utf-8')

Path('tests/test_wld_web_identity_contract.js').write_text('''"use strict";\n\nconst assert = require("node:assert/strict");\nconst crypto = require("node:crypto");\nconst test = require("node:test");\nconst { pathToFileURL } = require("node:url");\nconst path = require("node:path");\n\nconst ROOT = path.resolve(__dirname, "..");\nconst sha256 = (value) => crypto.createHash("sha256").update(value).digest("hex");\nconst hashExports = (values) => sha256(Buffer.from(`${values.join("\\n")}\\n`));\n\nfunction artifact(role, file) {\n  return { role, path: file, bytes: 1, sha256: "a".repeat(64) };\n}\n\ntest("WLD Web manifest requires build_info identity but not redundant ABI/capability exports", async () => {\n  const { validateManifest } = await import(pathToFileURL(path.join(ROOT, "scripts", "generate-manifest.mjs")));\n  const web = ["_terra_build_info_json", "_terra_world_open_begin"];\n  const node = ["_terra_abi_version", "_terra_capabilities", "_terra_build_info_json", "_terra_world_open_from_buffer"];\n  const webArtifacts = [artifact("wrapper", "build/web.js"), artifact("wasm", "build/web.wasm")];\n  const manifest = {\n    version: 1,\n    artifactId: "terrax-world-web",\n    sourceCommit: "0123456789abcdef0123456789abcdef01234567",\n    dirty: false,\n    abi: { version: 1, requiredExports: web, exportHash: hashExports(web) },\n    memory: { initialBytes: 67108864, maxBytes: 167772160 },\n    build: {\n      compiler: "emscripten",\n      featureSet: "wld",\n      flags: { common: ["-O3"], node: ["-sENVIRONMENT=node"], web: ["-sENVIRONMENT=web,worker"] },\n    },\n    targets: {\n      node: {\n        memory: { initialBytes: 134217728, maxBytes: 536870912 },\n        exports: node,\n        exportHash: hashExports(node),\n        artifacts: [artifact("wrapper", "build/node.js"), artifact("wasm", "build/node.wasm")],\n      },\n      web: {\n        memory: { initialBytes: 67108864, maxBytes: 167772160 },\n        exports: web,\n        exportHash: hashExports(web),\n        artifacts: webArtifacts,\n      },\n    },\n    artifacts: webArtifacts,\n  };\n\n  assert.doesNotThrow(() => validateManifest(manifest));\n  const missingBuildInfo = structuredClone(manifest);\n  missingBuildInfo.abi.requiredExports = ["_terra_world_open_begin"];\n  missingBuildInfo.abi.exportHash = hashExports(missingBuildInfo.abi.requiredExports);\n  missingBuildInfo.targets.web.exports = missingBuildInfo.abi.requiredExports;\n  missingBuildInfo.targets.web.exportHash = missingBuildInfo.abi.exportHash;\n  assert.throws(() => validateManifest(missingBuildInfo), /missing _terra_build_info_json/);\n});\n''', encoding='utf-8')
