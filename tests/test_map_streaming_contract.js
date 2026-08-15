"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");

test("MAP generation retains only bounded compressed bytes with an exact two-pass fallback", () => {
  const source = fs.readFileSync(path.join(ROOT, "src", "terra_map.c"), "utf8");

  assert.doesNotMatch(source, /MapChunkDesc/);
  assert.doesNotMatch(source, /TX_MAP_SINGLE_PASS_SLOT_BYTES/);
  assert.match(source, /TX_MAP_SINGLE_PASS_STAGING_LIMIT_BYTES\s+\(32u \* 1024u \* 1024u\)/);
  assert.match(source, /MapChunkStagingContext/);
  assert.match(source, /stage_map_chunk/);
  assert.match(source, /buf_reserve\(stage->compressed, size\)/);
  assert.match(source, /stage->compressed->len > stage->limit - size/);
  assert.match(source, /stage\.fallback/);

  // The exact measure/write implementation remains as the bounded fallback for
  // pathological MAPs whose compressed chunks exceed the single-pass staging cap.
  assert.match(source, /MapChunkMeasureContext/);
  assert.match(source, /MapChunkWriteContext/);
  assert.ok(
    (source.match(/walk_map_chunks\(/g) || []).length >= 4,
    "MAP source must contain one compressed-byte single-pass walk plus the exact fallback walks",
  );
  assert.match(source, /TX_MAP_MAX_OUTPUT_BYTES/);
});
