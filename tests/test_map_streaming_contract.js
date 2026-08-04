"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const ROOT = path.resolve(__dirname, "..");

test("MAP generation does not retain every compressed chunk", () => {
  const source = fs.readFileSync(path.join(ROOT, "src", "terra_map.c"), "utf8");

  assert.doesNotMatch(source, /MapChunkDesc/);
  assert.match(source, /MapChunkMeasureContext/);
  assert.match(source, /MapChunkWriteContext/);
  assert.ok(
    (source.match(/walk_map_chunks\(/g) || []).length >= 3,
    "MAP generation must measure and write chunks in separate passes",
  );
  assert.match(source, /TX_MAP_MAX_OUTPUT_BYTES/);
});

