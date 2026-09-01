from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8-sig")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one replacement, found {count}: {old[:120]!r}")
    write(path, text.replace(old, new, 1))


# ---------------------------------------------------------------------------
# PLR core: no numeric version gate + cached two-call outputs.
# ---------------------------------------------------------------------------
replace_once(
    "src/terra_plr.c",
    " * The on-disk payload is the current Terraria 318/319 player format:\n",
    " * The on-disk payload uses Terraria's modern encrypted player layout. The\n"
    " * version field is preserved as data rather than used as a numeric gate:\n"
    " * newer/older versions are accepted when their binary layout still matches.\n",
)
replace_once(
    "src/terra_plr.c",
    "#define PLR_MIN_VERSION 318\n#define PLR_MAX_VERSION 319\n",
    "",
)
replace_once(
    "src/terra_plr.c",
    "    int32_t version = plr_read_i32(&reader);\n"
    "    if (!reader.ok || version < PLR_MIN_VERSION || version > PLR_MAX_VERSION) return NULL;\n",
    "    int32_t version = plr_read_i32(&reader);\n"
    "    if (!reader.ok) return NULL;\n",
)
replace_once(
    "src/terra_plr.c",
    "    if (!plr_required_i32(root, \"version\")) return plr_model_error(\"PLR version is missing or invalid\");\n"
    "    int32_t version = 0;\n"
    "    if (!plr_value_i32(plr_json_object_get(root, \"version\"), &version) ||\n"
    "        version < PLR_MIN_VERSION || version > PLR_MAX_VERSION)\n"
    "        return plr_model_error(\"PLR version must be 318 or 319\");\n",
    "    if (!plr_required_i32(root, \"version\"))\n"
    "        return plr_model_error(\"PLR version is missing or invalid\");\n",
)
replace_once(
    "src/terra_plr.c",
    "typedef struct PlrDocumentSlot {\n"
    "    uint8_t active;\n"
    "    uint32_t handle;\n"
    "    PlrJsonValue *root;\n"
    "    uint8_t *original_encrypted;\n"
    "    uint32_t original_length;\n"
    "    uint8_t dirty;\n"
    "} PlrDocumentSlot;\n",
    "typedef struct PlrDocumentSlot {\n"
    "    uint8_t active;\n"
    "    uint32_t handle;\n"
    "    PlrJsonValue *root;\n"
    "    uint8_t *original_encrypted;\n"
    "    uint32_t original_length;\n"
    "    uint8_t *encoded_cache;\n"
    "    uint32_t encoded_cache_length;\n"
    "    uint8_t *json_cache;\n"
    "    uint32_t json_cache_length;\n"
    "    uint8_t dirty;\n"
    "} PlrDocumentSlot;\n",
)
replace_once(
    "src/terra_plr.c",
    "static void plr_destroy_document(PlrDocumentSlot *document) {\n"
    "    if (!document) return;\n"
    "    plr_json_free(document->root);\n"
    "    free(document->original_encrypted);\n"
    "    memset(document, 0, sizeof(*document));\n"
    "}\n",
    "static void plr_invalidate_document_caches(PlrDocumentSlot *document) {\n"
    "    if (!document) return;\n"
    "    free(document->encoded_cache);\n"
    "    document->encoded_cache = NULL;\n"
    "    document->encoded_cache_length = 0u;\n"
    "    free(document->json_cache);\n"
    "    document->json_cache = NULL;\n"
    "    document->json_cache_length = 0u;\n"
    "}\n\n"
    "static void plr_destroy_document(PlrDocumentSlot *document) {\n"
    "    if (!document) return;\n"
    "    plr_json_free(document->root);\n"
    "    free(document->original_encrypted);\n"
    "    plr_invalidate_document_caches(document);\n"
    "    memset(document, 0, sizeof(*document));\n"
    "}\n",
)
replace_once(
    "src/terra_plr.c",
    "    plr_json_free(document->root);\n"
    "    document->root = root;\n"
    "    free(document->original_encrypted);\n"
    "    document->original_encrypted = NULL;\n"
    "    document->original_length = 0u;\n"
    "    document->dirty = 1u;\n",
    "    plr_json_free(document->root);\n"
    "    document->root = root;\n"
    "    free(document->original_encrypted);\n"
    "    document->original_encrypted = NULL;\n"
    "    document->original_length = 0u;\n"
    "    plr_invalidate_document_caches(document);\n"
    "    document->dirty = 1u;\n",
)
replace_once(
    "src/terra_plr.c",
    "        g_plr_oom ? \"out of memory while parsing PLR\" : \"invalid or unsupported PLR payload\");\n",
    "        g_plr_oom ? \"out of memory while parsing PLR\" :\n"
    "            \"PLR payload is invalid or its version uses an incompatible binary layout\");\n",
)

encoded_old = '''static uint8_t *plr_encoded_document(
    const PlrDocumentSlot *document, uint32_t *out_length,
    terrax_world_status *out_status) {
    if (out_length) *out_length = 0u;
    if (out_status) *out_status = TERRAX_WORLD_STATUS_OK;
    if (!document || !document->root) {
        if (out_status) *out_status = plr_status_error(
            TERRAX_WORLD_STATUS_STATE_ERROR,
            "TERRAX_PLR_STATE_ERROR", "PLR document has no model");
        return NULL;
    }
    g_plr_oom = 0;
    if (!document->dirty && document->original_encrypted) {
        uint8_t *copy = (uint8_t *)plr_malloc(document->original_length);
        if (!copy) {
            if (out_status) *out_status = plr_status_error(
                TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
                "failed to allocate PLR output");
            return NULL;
        }
        memcpy(copy, document->original_encrypted, document->original_length);
        if (out_length) *out_length = document->original_length;
        return copy;
    }
    g_plr_oom = 0;
    uint32_t plain_length = 0u;
    uint8_t *plain = plr_encode_plain(document->root, &plain_length);
    if (!plain) {
        if (out_status) *out_status = g_plr_oom ?
            plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM", "out of memory while encoding PLR") :
            TERRAX_WORLD_STATUS_VALIDATION_ERROR;
        free(plain);
        return NULL;
    }
    uint32_t encrypted_length = 0u;
    uint8_t *encrypted = plr_encrypt(plain, plain_length, &encrypted_length);
    free(plain);
    if (!encrypted) {
        if (out_status) *out_status = plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
            "failed to encrypt PLR output");
        return NULL;
    }
    if (out_length) *out_length = encrypted_length;
    return encrypted;
}
'''
encoded_new = '''static terrax_world_status plr_copy_document_json_result(
    PlrDocumentSlot *document, char *buffer, uint64_t buffer_size,
    uint32_t *required_size) {
    if (!document || !document->root || !required_size) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INVALID_ARGUMENT,
            "TERRAX_INVALID_ARGUMENT",
            "null PLR document or required-size pointer");
    }
    if (!document->json_cache) {
        g_plr_oom = 0;
        uint32_t length = 0u;
        uint8_t *json = plr_json_serialize(document->root, &length);
        if (!json) {
            return plr_status_error(
                TERRAX_WORLD_STATUS_INTERNAL_ERROR,
                "TERRAX_WASM_OOM",
                "failed to serialize PLR JSON");
        }
        document->json_cache = json;
        document->json_cache_length = length;
    }
    uint64_t needed = (uint64_t)document->json_cache_length + 1u;
    if (needed > UINT32_MAX) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR,
            "TERRAX_WASM_OOM",
            "PLR JSON output exceeds the ABI size limit");
    }
    *required_size = (uint32_t)needed;
    if (!buffer || buffer_size == 0u) {
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (buffer_size < needed) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL,
            "TERRAX_BUFFER_TOO_SMALL",
            "PLR JSON output buffer is too small");
    }
    memcpy(buffer, document->json_cache, (unsigned long)needed);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
}

static const uint8_t *plr_encoded_document(
    PlrDocumentSlot *document, uint32_t *out_length,
    terrax_world_status *out_status) {
    if (out_length) *out_length = 0u;
    if (out_status) *out_status = TERRAX_WORLD_STATUS_OK;
    if (!document || !document->root) {
        if (out_status) *out_status = plr_status_error(
            TERRAX_WORLD_STATUS_STATE_ERROR,
            "TERRAX_PLR_STATE_ERROR", "PLR document has no model");
        return NULL;
    }
    if (!document->dirty && document->original_encrypted) {
        if (out_length) *out_length = document->original_length;
        return document->original_encrypted;
    }
    if (document->encoded_cache) {
        if (out_length) *out_length = document->encoded_cache_length;
        return document->encoded_cache;
    }
    g_plr_oom = 0;
    uint32_t plain_length = 0u;
    uint8_t *plain = plr_encode_plain(document->root, &plain_length);
    if (!plain) {
        if (out_status) *out_status = g_plr_oom ?
            plr_status_error(TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
                "out of memory while encoding PLR") :
            plr_status_error(TERRAX_WORLD_STATUS_VALIDATION_ERROR,
                "TERRAX_PLR_VALIDATION_ERROR",
                "PLR model cannot be encoded using the current binary layout");
        return NULL;
    }
    uint32_t encrypted_length = 0u;
    uint8_t *encrypted = plr_encrypt(plain, plain_length, &encrypted_length);
    free(plain);
    if (!encrypted) {
        if (out_status) *out_status = plr_status_error(
            TERRAX_WORLD_STATUS_INTERNAL_ERROR, "TERRAX_WASM_OOM",
            "failed to encrypt PLR output");
        return NULL;
    }
    document->encoded_cache = encrypted;
    document->encoded_cache_length = encrypted_length;
    if (out_length) *out_length = encrypted_length;
    return document->encoded_cache;
}
'''
replace_once("src/terra_plr.c", encoded_old, encoded_new)
replace_once(
    "src/terra_plr.c",
    "    g_plr_oom = 0;\n"
    "    return plr_copy_json_result(document->root, buffer, buffer_size, required_size);\n",
    "    g_plr_oom = 0;\n"
    "    return plr_copy_document_json_result(document, buffer, buffer_size, required_size);\n",
)

save_buffer_old = '''    terrax_world_status status = TERRAX_WORLD_STATUS_OK;
    uint32_t length = 0u;
    uint8_t *encoded = plr_encoded_document(document, &length, &status);
    if (!encoded) return status;
    *out_required = length;
    if (!output || capacity == 0u) {
        free(encoded);
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (capacity < length) {
        free(encoded);
        return plr_status_error(
            TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL,
            "TERRAX_BUFFER_TOO_SMALL",
            "PLR output buffer is too small");
    }
    memcpy(output, encoded, length);
    free(encoded);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
'''
save_buffer_new = '''    terrax_world_status status = TERRAX_WORLD_STATUS_OK;
    uint32_t length = 0u;
    const uint8_t *encoded = plr_encoded_document(document, &length, &status);
    if (!encoded) return status;
    *out_required = length;
    if (!output || capacity == 0u) {
        tx_clear_error();
        return TERRAX_WORLD_STATUS_OK;
    }
    if (capacity < length) {
        return plr_status_error(
            TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL,
            "TERRAX_BUFFER_TOO_SMALL",
            "PLR output buffer is too small");
    }
    memcpy(output, encoded, length);
    tx_clear_error();
    return TERRAX_WORLD_STATUS_OK;
'''
replace_once("src/terra_plr.c", save_buffer_old, save_buffer_new)
replace_once(
    "src/terra_plr.c",
    "    uint8_t *encoded = plr_encoded_document(document, &length, &status);\n"
    "    if (!encoded) return status;\n"
    "    int ok = plr_write_file(path_utf8, encoded, length);\n"
    "    free(encoded);\n",
    "    const uint8_t *encoded = plr_encoded_document(document, &length, &status);\n"
    "    if (!encoded) return status;\n"
    "    int ok = plr_write_file(path_utf8, encoded, length);\n",
)

# ---------------------------------------------------------------------------
# Real fixture contracts: keep synthetic creation tests, but make real PLR
# parsing/writing a separate mandatory CI gate.
# ---------------------------------------------------------------------------
replace_once(
    "CMakeLists.txt",
    '''            target_compile_definitions(terra_plr_contract PRIVATE
                TERRAWASM_PLR_JSON_FIXTURE_PATH="${TERRAWASM_PLR_JSON_FIXTURE_DEFINE}"
            )
''',
    '''            target_compile_definitions(terra_plr_contract PRIVATE
                TERRAWASM_PLR_JSON_FIXTURE_PATH="${TERRAWASM_PLR_JSON_FIXTURE_DEFINE}"
            )

            set(TERRAWASM_PLR_REAL_FIXTURE_PATH
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/files/烟花.plr"
                CACHE FILEPATH "Real Terraria PLR fixture used by native compatibility tests")
            if(NOT EXISTS "${TERRAWASM_PLR_REAL_FIXTURE_PATH}")
                message(FATAL_ERROR
                    "Real PLR compatibility fixture is missing: ${TERRAWASM_PLR_REAL_FIXTURE_PATH}")
            endif()
            file(TO_CMAKE_PATH
                "${TERRAWASM_PLR_REAL_FIXTURE_PATH}"
                TERRAWASM_PLR_REAL_FIXTURE_DEFINE)
            add_terra_contract(terra_plr_real_fixture_contract tests/plr_real_fixture_contract.c)
            target_compile_definitions(terra_plr_real_fixture_contract PRIVATE
                TERRAWASM_PLR_REAL_FIXTURE_PATH="${TERRAWASM_PLR_REAL_FIXTURE_DEFINE}"
            )
''',
)
replace_once(
    "build.ps1",
    '    if ($Features -ne "wld") { $regressionTests += "tests/test_plr.js" }',
    '    if ($Features -ne "wld") { $regressionTests += @("tests/test_plr.js", "tests/test_plr_real_fixture.js") }',
)

real_js = r'''"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const TerraWorldWasm = require(path.join(__dirname, "..", "build", "terrax_world_wasm.js"));
const TerraWorldWasmWeb = require(path.join(__dirname, "..", "build", "terrax_world_wasm_web.js"));

const REAL_FIXTURE = process.env.TERRAWASM_PLR_FIXTURE ||
  path.join(__dirname, "files", "烟花.plr");
const MODEL_FIXTURE = path.join(__dirname, "fixtures", "minimal-player.json");

function allocBytes(module, bytes) {
  const value = Buffer.from(bytes);
  const pointer = module._tx_malloc(Math.max(1, value.length));
  assert(pointer, "tx_malloc failed");
  module.HEAPU8.set(value, pointer);
  return pointer;
}

function allocString(module, value) {
  return allocBytes(module, Buffer.from(`${value}\0`, "utf8"));
}

function openBuffer(module, bytes) {
  const input = allocBytes(module, bytes);
  const output = module._tx_malloc(4);
  assert(output, "handle allocation failed");
  try {
    assert.equal(module._terra_player_open_from_buffer(input, bytes.length, output), 0,
      "real Terraria PLR fixture was rejected");
    const handle = module.HEAPU32[output >>> 2] >>> 0;
    assert(handle, "PLR handle was not returned");
    return handle;
  } finally {
    module._tx_free(output);
    module._tx_free(input);
  }
}

function openJson(module, value) {
  const input = allocString(module, JSON.stringify(value));
  const output = module._tx_malloc(4);
  assert(output);
  try {
    assert.equal(module._terra_player_open_json(input, output), 0);
    const handle = module.HEAPU32[output >>> 2] >>> 0;
    assert(handle);
    return handle;
  } finally {
    module._tx_free(output);
    module._tx_free(input);
  }
}

function probeJsonCache(module, handle) {
  const required = module._tx_malloc(4);
  assert(required);
  try {
    const before = module._tx_heap_used();
    assert.equal(module._terra_player_get_json(handle, 0, 0n, required), 0);
    const afterFirst = module._tx_heap_used();
    assert(afterFirst > before, "first get_json probe should populate the document JSON cache");
    assert.equal(module._terra_player_get_json(handle, 0, 0n, required), 0);
    assert.equal(module._tx_heap_used(), afterFirst,
      "second get_json probe must reuse the serialized JSON cache");
    return module.HEAPU32[required >>> 2] >>> 0;
  } finally {
    module._tx_free(required);
  }
}

function getDocumentJson(module, handle, knownSize = 0) {
  const required = module._tx_malloc(4);
  assert(required);
  let output = 0;
  try {
    let size = knownSize;
    if (!size) {
      assert.equal(module._terra_player_get_json(handle, 0, 0n, required), 0);
      size = module.HEAPU32[required >>> 2] >>> 0;
    }
    assert(size > 1);
    output = module._tx_malloc(size);
    assert(output);
    const beforeFetch = module._tx_heap_used();
    assert.equal(module._terra_player_get_json(handle, output, BigInt(size), required), 0);
    assert.equal(module._tx_heap_used(), beforeFetch,
      "get_json fetch must not serialize the document again");
    return JSON.parse(module.UTF8ToString(output, size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

function getField(module, handle, pointer) {
  const pathBuffer = allocString(module, pointer);
  const required = module._tx_malloc(4);
  let output = 0;
  try {
    assert.equal(module._terra_player_get_field_json(handle, pathBuffer, 0, 0n, required), 0);
    const size = module.HEAPU32[required >>> 2] >>> 0;
    output = module._tx_malloc(size);
    assert(output);
    assert.equal(module._terra_player_get_field_json(
      handle, pathBuffer, output, BigInt(size), required), 0);
    return JSON.parse(module.UTF8ToString(output, size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
    module._tx_free(pathBuffer);
  }
}

function setField(module, handle, pointer, value) {
  const pathBuffer = allocString(module, pointer);
  const valueBuffer = allocString(module, JSON.stringify(value));
  try {
    return module._terra_player_set_field_json(handle, pathBuffer, valueBuffer);
  } finally {
    module._tx_free(valueBuffer);
    module._tx_free(pathBuffer);
  }
}

function encode(module, handle, { expectCached = false } = {}) {
  const required = module._tx_malloc(4);
  let output = 0;
  try {
    const beforeProbe = module._tx_heap_used();
    assert.equal(module._terra_player_save_to_buffer(handle, 0, 0, required), 0);
    const afterFirstProbe = module._tx_heap_used();
    if (expectCached) {
      assert(afterFirstProbe > beforeProbe,
        "first dirty save probe should populate the encrypted output cache");
    } else {
      assert.equal(afterFirstProbe, beforeProbe,
        "clean save probe should use original encrypted bytes without allocating");
    }
    assert.equal(module._terra_player_save_to_buffer(handle, 0, 0, required), 0);
    assert.equal(module._tx_heap_used(), afterFirstProbe,
      "second save probe must reuse the existing encrypted bytes/cache");
    const size = module.HEAPU32[required >>> 2] >>> 0;
    assert(size > 0);
    output = module._tx_malloc(size);
    assert(output);
    const beforeFetch = module._tx_heap_used();
    assert.equal(module._terra_player_save_to_buffer(handle, output, size, required), 0);
    assert.equal(module._tx_heap_used(), beforeFetch,
      "save fetch must not encode/encrypt the PLR again");
    return Buffer.from(module.HEAPU8.slice(output, output + size));
  } finally {
    if (output) module._tx_free(output);
    module._tx_free(required);
  }
}

function assertSemanticShape(player) {
  assert.equal(typeof player.name, "string");
  assert(player.name.length > 0, "real player name is empty");
  assert(Number.isInteger(player.version), "player version is not an integer");
  assert.equal(player.inventory.length, 58);
  assert.equal(player.armor.length, 20);
  assert.equal(player.dyes.length, 10);
  assert.equal(player.buffs.length, 44);
  assert.equal(player.loadouts.length, 3);
}

function realFixtureBytes() {
  assert.equal(fs.existsSync(REAL_FIXTURE), true,
    `real PLR fixture is required: ${REAL_FIXTURE}`);
  const source = fs.readFileSync(REAL_FIXTURE);
  assert(source.length >= 16 && source.length % 16 === 0,
    "real PLR fixture is not AES-CBC framed");
  return source;
}

test("Node opens a real Terraria PLR and reuses two-call caches", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const source = realFixtureBytes();
  let handle = 0;
  let reopened = 0;
  try {
    handle = openBuffer(module, source);
    const jsonSize = probeJsonCache(module, handle);
    const original = getDocumentJson(module, handle, jsonSize);
    assertSemanticShape(original);

    const clean = encode(module, handle);
    assert.deepEqual(clean, source,
      "clean real-player save must preserve the exact encrypted bytes");

    const editedName = `${original.name}-terrawasm`;
    assert.equal(setField(module, handle, "/name", editedName), 0);
    const edited = encode(module, handle, { expectCached: true });
    assert.notDeepEqual(edited, source);
    reopened = openBuffer(module, edited);
    assert.equal(getField(module, reopened, "/name"), editedName);
    assert.equal(getField(module, reopened, "/version"), original.version);
  } finally {
    if (reopened) assert.equal(module._terra_player_close(reopened), 0);
    if (handle) assert.equal(module._terra_player_close(handle), 0);
  }
  assert.equal(module._tx_heap_used(), baseline,
    "real PLR handles/caches leaked native allocations");
});

test("Web opens and edits the real Terraria PLR fixture", async () => {
  const module = await TerraWorldWasmWeb({
    wasmBinary: fs.readFileSync(path.join(__dirname, "..", "build", "terrax_world_wasm_web.wasm")),
  });
  const baseline = module._tx_heap_used();
  const source = realFixtureBytes();
  let handle = 0;
  let reopened = 0;
  try {
    handle = openBuffer(module, source);
    const originalName = getField(module, handle, "/name");
    assert.equal(typeof originalName, "string");
    const clean = encode(module, handle);
    assert.deepEqual(clean, source);
    assert.equal(setField(module, handle, "/name", `${originalName}-web`), 0);
    reopened = openBuffer(module, encode(module, handle, { expectCached: true }));
    assert.equal(getField(module, reopened, "/name"), `${originalName}-web`);
  } finally {
    if (reopened) module._terra_player_close(reopened);
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});

test("PLR version is not hard-gated when the binary layout is compatible", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  model.version = 777;
  let handle = 0;
  let reopened = 0;
  try {
    handle = openJson(module, model);
    assert.equal(getField(module, handle, "/version"), 777);
    reopened = openBuffer(module, encode(module, handle, { expectCached: true }));
    assert.equal(getField(module, reopened, "/version"), 777);
  } finally {
    if (reopened) module._terra_player_close(reopened);
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});
'''
write("tests/test_plr_real_fixture.js", real_js)

real_c = r'''#include "terra_plr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef TERRAWASM_PLR_REAL_FIXTURE_PATH
#error "TERRAWASM_PLR_REAL_FIXTURE_PATH must point to a real Terraria .plr fixture"
#endif

extern uint32_t tx_heap_used(void);

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "PLR real-fixture contract failure: %s\n", (message)); \
        return 1; \
    } \
} while (0)

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t length) {
    if (!a || !b) return length == 0u;
    for (uint32_t i = 0u; i < length; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int text_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    uint32_t i = 0u;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return 1;
}

static uint8_t *read_fixture(uint32_t *out_length) {
    *out_length = 0u;
    FILE *file = fopen(TERRAWASM_PLR_REAL_FIXTURE_PATH, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || (uint64_t)size >= UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(data, 1u, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    *out_length = (uint32_t)size;
    return data;
}

static int get_field(uint32_t handle, const char *pointer, char **out) {
    uint32_t required = 0u;
    if (terra_plr_get(handle, pointer, NULL, 0u, &required) != TERRAX_WORLD_STATUS_OK ||
        required < 2u) return 0;
    char *text = (char *)malloc(required);
    if (!text) return 0;
    if (terra_plr_get(handle, pointer, text, required, &required) != TERRAX_WORLD_STATUS_OK) {
        free(text);
        return 0;
    }
    *out = text;
    return 1;
}

int main(void) {
    uint32_t fixture_length = 0u;
    uint8_t *fixture = read_fixture(&fixture_length);
    CHECK(fixture != NULL, "real fixture could not be read");
    CHECK(fixture_length >= 16u && (fixture_length & 15u) == 0u,
        "real fixture is not AES-CBC framed");

    uint32_t baseline = tx_heap_used();
    uint32_t handle = 0u;
    CHECK(terra_plr_open_from_buffer(fixture, fixture_length, &handle) ==
        TERRAX_WORLD_STATUS_OK, "real Terraria PLR did not open");
    CHECK(handle != 0u, "real fixture returned null handle");

    uint32_t json_required = 0u;
    uint32_t before_json = tx_heap_used();
    CHECK(terra_plr_get_json(handle, NULL, 0u, &json_required) ==
        TERRAX_WORLD_STATUS_OK && json_required > 1u, "get_json probe");
    uint32_t after_json = tx_heap_used();
    CHECK(after_json > before_json, "first JSON probe did not populate cache");
    CHECK(terra_plr_get_json(handle, NULL, 0u, &json_required) ==
        TERRAX_WORLD_STATUS_OK, "second get_json probe");
    CHECK(tx_heap_used() == after_json, "second JSON probe rebuilt the cache");

    char *name = NULL;
    char *version = NULL;
    CHECK(get_field(handle, "/name", &name), "read real player name");
    CHECK(name[0] == '"' && name[1] != '"', "real player name is empty");
    CHECK(get_field(handle, "/version", &version), "read real player version");

    uint32_t required = 0u;
    uint32_t before_clean_probe = tx_heap_used();
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK && required == fixture_length, "clean save probe");
    CHECK(tx_heap_used() == before_clean_probe,
        "clean save probe allocated instead of using original encrypted bytes");
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK, "second clean save probe");
    CHECK(tx_heap_used() == before_clean_probe, "second clean save probe allocated");

    uint8_t *clean = (uint8_t *)malloc(required);
    CHECK(clean != NULL, "clean output allocation");
    CHECK(terra_plr_save_to_buffer(handle, clean, required, &required) ==
        TERRAX_WORLD_STATUS_OK, "clean save fetch");
    CHECK(bytes_equal(clean, fixture, fixture_length),
        "clean real-player save did not preserve exact encrypted bytes");
    free(clean);

    CHECK(terra_plr_set(handle, "/name", "\"real-fixture-edited\"") ==
        TERRAX_WORLD_STATUS_OK, "edit real fixture name");
    uint32_t before_dirty_probe = tx_heap_used();
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK && required > 0u, "dirty save probe");
    uint32_t after_dirty_probe = tx_heap_used();
    CHECK(after_dirty_probe > before_dirty_probe, "dirty save did not populate encoded cache");
    CHECK(terra_plr_save_to_buffer(handle, NULL, 0u, &required) ==
        TERRAX_WORLD_STATUS_OK, "second dirty save probe");
    CHECK(tx_heap_used() == after_dirty_probe, "dirty save probe re-encoded the PLR");

    uint8_t *edited = (uint8_t *)malloc(required);
    CHECK(edited != NULL, "edited output allocation");
    uint32_t edited_length = required;
    CHECK(terra_plr_save_to_buffer(handle, edited, edited_length, &required) ==
        TERRAX_WORLD_STATUS_OK, "dirty save fetch");
    CHECK(tx_heap_used() == after_dirty_probe, "dirty save fetch re-encoded the PLR");

    uint32_t reopened = 0u;
    CHECK(terra_plr_open_from_buffer(edited, edited_length, &reopened) ==
        TERRAX_WORLD_STATUS_OK, "edited real fixture did not reopen");
    char *edited_name = NULL;
    CHECK(get_field(reopened, "/name", &edited_name), "read edited name");
    CHECK(text_equal(edited_name, "\"real-fixture-edited\""),
        "edited name did not survive semantic encoding");

    /* Version is data, not a whitelist. If the binary schema still matches,
     * an otherwise-valid document must round-trip regardless of version value. */
    CHECK(terra_plr_set(reopened, "/version", "777") == TERRAX_WORLD_STATUS_OK,
        "arbitrary compatible version was rejected");
    uint32_t versioned_length = 0u;
    CHECK(terra_plr_save_to_buffer(reopened, NULL, 0u, &versioned_length) ==
        TERRAX_WORLD_STATUS_OK && versioned_length > 0u, "versioned save probe");
    uint8_t *versioned = (uint8_t *)malloc(versioned_length);
    CHECK(versioned != NULL, "versioned output allocation");
    CHECK(terra_plr_save_to_buffer(reopened, versioned, versioned_length, &versioned_length) ==
        TERRAX_WORLD_STATUS_OK, "versioned save fetch");
    uint32_t versioned_handle = 0u;
    CHECK(terra_plr_open_from_buffer(versioned, versioned_length, &versioned_handle) ==
        TERRAX_WORLD_STATUS_OK, "compatible version 777 was hard-gated on read");
    char *version_777 = NULL;
    CHECK(get_field(versioned_handle, "/version", &version_777), "read version 777");
    CHECK(text_equal(version_777, "777"), "version 777 was not preserved");

    free(version_777);
    free(versioned);
    free(edited_name);
    free(edited);
    free(version);
    free(name);
    CHECK(terra_plr_close(versioned_handle) == TERRAX_WORLD_STATUS_OK, "close versioned handle");
    CHECK(terra_plr_close(reopened) == TERRAX_WORLD_STATUS_OK, "close reopened handle");
    CHECK(terra_plr_close(handle) == TERRAX_WORLD_STATUS_OK, "close real fixture handle");
    CHECK(tx_heap_used() == baseline, "real PLR contract leaked tracked allocations/caches");
    free(fixture);
    return 0;
}
'''
write("tests/plr_real_fixture_contract.c", real_c)

# ---------------------------------------------------------------------------
# Public docs: parser is schema/layout based rather than 318/319 whitelisted.
# ---------------------------------------------------------------------------
replace_once(
    "README.md",
    "- **玩家文件**：读取、编辑并写回 Terraria 318/319 的加密 `.plr`，支持 JSON Pointer 和结构化补丁",
    "- **玩家文件**：读取、编辑并写回 Terraria 加密 `.plr`，支持 JSON Pointer 和结构化补丁；不按版本号硬编码白名单，而以实际二进制布局是否兼容为准",
)
replace_once(
    "README.md",
    "PLR 使用 Terraria 318/319 的 AES-128-CBC + PKCS#7 格式，密钥/IV 为 UTF-16LE `h3y_gUyZ`。`terra_plr_*` 是主命名空间，`terra_player_*` 是兼容 TerraR 的别名。JSON 结果使用 UTF-8；JSON 查询遵循 RFC 6901，例如 `/inventory/0/stack`。",
    "PLR 使用 Terraria 的 AES-128-CBC + PKCS#7 加密封装，密钥/IV 为 UTF-16LE `h3y_gUyZ`。解析器不再把 318/319 作为硬性版本白名单：它会保留文件中的 `version` 并按当前语义布局尝试解析；布局不兼容或数据损坏时返回解析错误。`terra_plr_*` 是主命名空间，`terra_player_*` 是兼容 TerraR 的别名。JSON 结果使用 UTF-8；JSON 查询遵循 RFC 6901，例如 `/inventory/0/stack`。",
)
replace_once(
    "docs/API.md",
    "`terra_plr_*` 提供 Terraria 318/319 加密 `.plr` 的读写和语义编辑；`terra_player_*` 是同签名的兼容别名。文件使用 AES-128-CBC/PKCS#7，密钥和 IV 均为 UTF-16LE `h3y_gUyZ`。二进制输入经过严格的 metadata magic/type、版本、长度和尾部校验。",
    "`terra_plr_*` 提供 Terraria 加密 `.plr` 的读写和语义编辑；`terra_player_*` 是同签名的兼容别名。文件使用 AES-128-CBC/PKCS#7，密钥和 IV 均为 UTF-16LE `h3y_gUyZ`。版本号不使用固定白名单：解析器保留 `version`，并依靠 metadata magic/type、字段边界、长度、计数和尾部完整性判断当前二进制布局是否兼容；布局不匹配时返回解析错误。",
)

# The helper is intentionally self-removing: only the source/test changes stay
# on the review branch after the one-shot Actions transformation.
(ROOT / ".github/workflows/apply-plr-review-fixes.yml").unlink(missing_ok=True)
Path(__file__).unlink(missing_ok=True)
