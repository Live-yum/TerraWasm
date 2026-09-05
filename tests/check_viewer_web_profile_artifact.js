"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const wasmPath = path.resolve(process.argv[2] || path.join(__dirname, "..", "build", "terrax_world_wasm_web.wasm"));
const bytes = fs.readFileSync(wasmPath);

assert.equal(WebAssembly.validate(bytes), true, "expected a valid WebAssembly binary");

function readU32Leb(buffer, start) {
  let value = 0;
  let shift = 0;
  let offset = start;
  for (;;) {
    assert.ok(offset < buffer.length, "truncated WebAssembly LEB128 value");
    const byte = buffer[offset++];
    value |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return { value: value >>> 0, offset };
    shift += 7;
    assert.ok(shift < 35, "invalid WebAssembly u32 LEB128 value");
  }
}

function collectSectionPayloads(buffer, sectionId) {
  const payloads = [];
  let offset = 8;
  while (offset < buffer.length) {
    const id = buffer[offset++];
    const decoded = readU32Leb(buffer, offset);
    const size = decoded.value;
    offset = decoded.offset;
    const end = offset + size;
    assert.ok(end <= buffer.length, `truncated WebAssembly section ${id}`);
    if (id === sectionId) payloads.push(buffer.subarray(offset, end));
    offset = end;
  }
  assert.equal(offset, buffer.length, "WebAssembly section parser ended off boundary");
  return payloads;
}

const runtimeData = Buffer.concat(collectSectionPayloads(bytes, 11));
assert.ok(runtimeData.length > 0, "viewer Web artifact must contain a runtime data section");
const hasRuntimeString = (value) => runtimeData.includes(Buffer.from(value, "utf8"));

// These operation literals must remain reachable by the viewer dispatcher.
for (const retained of [
  "render_preview_png",
  "render_thumbnail_png",
  "render_lit_map",
  "mark_tiles_and_chests_preview",
  "mark_tiles_and_chests_map",
  "batch_update_tiles",
  "header_patch",
  "replace_chests",
  "replace_bestiary",
]) {
  assert.equal(hasRuntimeString(retained), true, `${retained} missing from viewer Web runtime data`);
}

// Build identity is compiled into the artifact and independently proves that
// this is the viewer-profile build rather than the generic WLD Web artifact.
assert.equal(
  hasRuntimeString("viewerWebProfile"),
  true,
  "viewer Web artifact identity must advertise viewerWebProfile",
);
assert.equal(
  hasRuntimeString('"viewerWebProfile":true'),
  true,
  "viewer Web artifact identity must serialize viewerWebProfile as a JSON boolean",
);

// apply_pixel_art_mapping is fully retired rather than merely profile-gated,
// so it is safe to require that its dispatcher literal is gone entirely.
assert.equal(
  hasRuntimeString("apply_pixel_art_mapping"),
  false,
  "retired apply_pixel_art_mapping literal leaked into viewer Web runtime data",
);

// Do not require every profile-gated operation name to disappear byte-for-byte.
// Names such as render_preview_rgba can remain reachable through generic
// rendering/API code even when their JSON dispatcher branch is compiled out.
// Exact dispatcher guards are verified by test_viewer_web_profile_contract.js,
// while CI separately requires the profile .wasm to be strictly smaller than
// the generic WLD Web build.
console.log(
  `viewer Web profile artifact contract passed: ${wasmPath} (${bytes.length} bytes, runtimeData=${runtimeData.length} bytes)`,
);
