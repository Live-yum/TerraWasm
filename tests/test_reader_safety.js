"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const {
  CORRUPTED_FIXTURE_DIR,
  TRUNCATED_FIXTURE_DIR,
  readFixtureDirectory,
} = require("./helpers/fixtures");

const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));

function alloc(M, bytes) {
  const ptr = M._tx_malloc(bytes.length);
  assert.notEqual(ptr, 0);
  M.HEAPU8.set(bytes, ptr);
  return ptr;
}

test("truncated and corrupted world buffers fail as statuses without crashing", async () => {
  const M = await TerraWorldWasm();
  const candidates = [
    ...readFixtureDirectory(TRUNCATED_FIXTURE_DIR).map((fixturePath) => fs.readFileSync(fixturePath)),
    ...readFixtureDirectory(CORRUPTED_FIXTURE_DIR).map((fixturePath) => fs.readFileSync(fixturePath)),
    Uint8Array.from({ length: 4096 }, (_, index) => (index * 73) & 0xff),
    crypto.randomBytes(4096),
  ];
  for (const candidate of candidates) {
    const inputPtr = alloc(M, candidate);
    const task = M._terra_world_open_begin(inputPtr, candidate.length);
    assert.notEqual(task, 0);
    const status = M._terra_world_open_step(task, 1);
    assert.notEqual(status, 0, `unexpected success for ${candidate.length}-byte invalid input`);
    assert.ok(status >= 1 && status <= 11);
    assert.equal(M._terra_world_task_get_world_handle(task), 0);
    assert.equal(M._terra_world_task_close(task), 0);
    M._tx_free(inputPtr);
  }
});
