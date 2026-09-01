from pathlib import Path
import re


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 exact match, got {count}")
    return text.replace(old, new, 1)


def replace_count(text, old, new, expected, label):
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{label}: expected {expected} exact matches, got {count}")
    return text.replace(old, new)


plr_path = Path("src/terra_plr.c")
plr = plr_path.read_text(encoding="utf-8")

plr = replace_once(plr, '''static int plr_version_uses_v326_layout(int32_t version) {
    /* v326 extends persisted equipment slots with a favorite byte and adds
     * one body-prefix boolean. Higher versions are attempted using the most
     * recent known layout; the strict EOF check rejects incompatible layouts. */
    return version >= 326;
}
''', '''#define PLR_MIN_SUPPORTED_VERSION 318
#define PLR_CURRENT_VERSION 326

static int plr_version_supported(int32_t version) {
    return version >= PLR_MIN_SUPPORTED_VERSION && version <= PLR_CURRENT_VERSION;
}

static int plr_version_has_equipment_favorites(int32_t version) {
    /* Player.LoadPlayer_Version2 and EquipmentLoadout.Deserialize start
     * persisting armor/dye favorites at release 322. */
    return version >= 322;
}

static int plr_version_has_reserved_324(int32_t version) {
    /* Player.SavePlayer writes a reserved false boolean immediately after
     * ateArtisanBread; Player.LoadPlayer_Version2 consumes it from release 324. */
    return version >= 324;
}
''', "replace PLR version helpers")

plr = replace_count(plr,
'''    const int v326_layout = plr_version_uses_v326_layout(version);
''',
'''    if (!plr_version_supported(version)) return NULL;
    const int equipment_favorites = plr_version_has_equipment_favorites(version);
    const int reserved_324 = plr_version_has_reserved_324(version);
''', 2, "replace parser/writer version state")

old_body_reader = '''    if (!plr_put_reader_u8(root, "hideMisc", &reader) ||
        !plr_put_reader_u8(root, "skinVariant", &reader) ||
        !plr_put_reader_i32(root, "statLife", &reader) ||
        !plr_put_reader_i32(root, "statLifeMax", &reader) ||
        !plr_put_reader_i32(root, "statMana", &reader) ||
        !plr_put_reader_i32(root, "statManaMax", &reader) ||
        !plr_put_reader_bool(root, "extraAccessory", &reader) ||
        !plr_put_reader_bool(root, "unlockedBiomeTorches", &reader) ||
        !plr_put_reader_bool(root, "usingBiomeTorches", &reader) ||
        !plr_put_reader_bool(root, "ateArtisanBread", &reader) ||
        !plr_put_reader_bool(root, "usedAegisCrystal", &reader) ||
        !plr_put_reader_bool(root, "usedAegisFruit", &reader) ||
        !plr_put_reader_bool(root, "usedArcaneCrystal", &reader) ||
        !plr_put_reader_bool(root, "usedGalaxyPearl", &reader) ||
        !plr_put_reader_bool(root, "usedGummyWorm", &reader) ||
        !plr_put_reader_bool(root, "usedAmbrosia", &reader) ||
        !plr_put_reader_bool(root, "downedDd2EventAnyDifficulty", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }
    if (v326_layout) {
        int extension_flag = plr_read_u8(&reader) != 0u;
        PlrJsonValue *extensions = plr_json_object();
        if (!reader.ok || !extensions ||
            !plr_json_object_put_bool(extensions, "v326PrefixFlag", extension_flag)) {
            plr_json_free(extensions);
            plr_json_free(root);
            return NULL;
        }
        /* plr_root_put owns extensions on both success and insertion failure. */
        if (!plr_root_put(root, "formatExtensions", extensions)) {
            plr_json_free(root);
            return NULL;
        }
        extensions = NULL;
    }
'''
new_body_reader = '''    if (!plr_put_reader_u8(root, "hideMisc", &reader) ||
        !plr_put_reader_u8(root, "skinVariant", &reader) ||
        !plr_put_reader_i32(root, "statLife", &reader) ||
        !plr_put_reader_i32(root, "statLifeMax", &reader) ||
        !plr_put_reader_i32(root, "statMana", &reader) ||
        !plr_put_reader_i32(root, "statManaMax", &reader) ||
        !plr_put_reader_bool(root, "extraAccessory", &reader) ||
        !plr_put_reader_bool(root, "unlockedBiomeTorches", &reader) ||
        !plr_put_reader_bool(root, "usingBiomeTorches", &reader) ||
        !plr_put_reader_bool(root, "ateArtisanBread", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }
    if (reserved_324) {
        /* Terraria intentionally discards this reserved boolean. Do not expose
         * it as semantic player state. BinaryReader.ReadBoolean accepts any
         * nonzero byte, so consuming one bounded byte is the compatible form. */
        (void)plr_read_u8(&reader);
        if (!reader.ok) {
            plr_json_free(root);
            return NULL;
        }
    }
    if (!plr_put_reader_bool(root, "usedAegisCrystal", &reader) ||
        !plr_put_reader_bool(root, "usedAegisFruit", &reader) ||
        !plr_put_reader_bool(root, "usedArcaneCrystal", &reader) ||
        !plr_put_reader_bool(root, "usedGalaxyPearl", &reader) ||
        !plr_put_reader_bool(root, "usedGummyWorm", &reader) ||
        !plr_put_reader_bool(root, "usedAmbrosia", &reader) ||
        !plr_put_reader_bool(root, "downedDd2EventAnyDifficulty", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }
'''
plr = replace_once(plr, old_body_reader, new_body_reader, "move reserved v324 byte")

plr = replace_count(plr, "v326_layout", "equipment_favorites", 5,
                    "replace remaining v326 equipment gates")

old_validation = '''    if (!plr_value_i32(plr_json_object_get(root, "version"), &model_version))
        return plr_model_error("PLR version is missing or invalid");
    const PlrJsonValue *format_extensions =
        plr_json_object_get(root, "formatExtensions");
    if (format_extensions) {
        if (!plr_version_uses_v326_layout(model_version) ||
            format_extensions->type != PLR_JSON_OBJECT ||
            !plr_required_bool(format_extensions, "v326PrefixFlag"))
            return plr_model_error("PLR formatExtensions is invalid for this version");
    }
'''
new_validation = '''    if (!plr_value_i32(plr_json_object_get(root, "version"), &model_version))
        return plr_model_error("PLR version is missing or invalid");
    if (!plr_version_supported(model_version))
        return plr_model_error("PLR version must be within the supported Terraria 318-326 range");
'''
plr = replace_once(plr, old_validation, new_validation, "align PLR validation range")

old_body_writer = '''    const char *body_bool_fields[] = {
        "extraAccessory", "unlockedBiomeTorches", "usingBiomeTorches",
        "ateArtisanBread", "usedAegisCrystal", "usedAegisFruit",
        "usedArcaneCrystal", "usedGalaxyPearl", "usedGummyWorm",
        "usedAmbrosia", "downedDd2EventAnyDifficulty"
    };
    for (uint32_t index = 0u; index < 11u; index++) {
        if (!plr_field_bool(root, body_bool_fields[index], &boolean)) writer.ok = 0;
        plr_writer_u8(&writer, boolean ? 1u : 0u);
    }
    if (equipment_favorites) {
        int extension_flag = 0;
        const PlrJsonValue *extensions = plr_field(root, "formatExtensions");
        if (extensions) {
            if (extensions->type != PLR_JSON_OBJECT ||
                !plr_field_bool(extensions, "v326PrefixFlag", &extension_flag))
                writer.ok = 0;
        }
        plr_writer_u8(&writer, extension_flag ? 1u : 0u);
    }
'''
new_body_writer = '''    const char *body_bool_prefix_fields[] = {
        "extraAccessory", "unlockedBiomeTorches", "usingBiomeTorches", "ateArtisanBread"
    };
    for (uint32_t index = 0u; index < 4u; index++) {
        if (!plr_field_bool(root, body_bool_prefix_fields[index], &boolean)) writer.ok = 0;
        plr_writer_u8(&writer, boolean ? 1u : 0u);
    }
    if (reserved_324) {
        /* Match Player.SavePlayer exactly: the reserved release-324 byte is
         * always written as false and is not editable semantic state. */
        plr_writer_u8(&writer, 0u);
    }
    const char *body_bool_suffix_fields[] = {
        "usedAegisCrystal", "usedAegisFruit", "usedArcaneCrystal",
        "usedGalaxyPearl", "usedGummyWorm", "usedAmbrosia",
        "downedDd2EventAnyDifficulty"
    };
    for (uint32_t index = 0u; index < 7u; index++) {
        if (!plr_field_bool(root, body_bool_suffix_fields[index], &boolean)) writer.ok = 0;
        plr_writer_u8(&writer, boolean ? 1u : 0u);
    }
'''
plr = replace_once(plr, old_body_writer, new_body_writer, "align reserved v324 writer")

if "v326_layout" in plr or "v326PrefixFlag" in plr or "plr_version_uses_v326_layout" in plr:
    raise SystemExit("stale v326 inferred-layout identifiers remain in terra_plr.c")
plr_path.write_text(plr, encoding="utf-8")

wld_path = Path("src/terra_wld.c")
wld = wld_path.read_text(encoding="utf-8")
wld = replace_once(wld,
    "if (w->version<88u||w->version>400u){",
    "if (w->version<88u||w->version>326u){",
    "align WLD current-version upper bound")
wld_path.write_text(wld, encoding="utf-8")

# Real-v326 JS contract: remove the invented semantic extension and replace
# permissive future-version behavior with Terraria's current-version guard.
real_js_path = Path("tests/test_plr_real_fixture.js")
real_js = real_js_path.read_text(encoding="utf-8")
real_js = replace_once(real_js, '''    assert.equal(original.formatExtensions?.v326PrefixFlag, true,
      "v326 body-prefix extension byte was not preserved");
''', '''    assert.equal(original.formatExtensions, undefined,
      "reserved release-324 byte must not leak into semantic player JSON");
''', "update real Node reserved-byte assertion")
real_js = replace_once(real_js, '''    assert.equal(getField(module, handle, "/formatExtensions/v326PrefixFlag"), true);
''', '', "remove real Web invented extension assertion")
old_future_js = '''test("PLR version is not hard-gated when the binary layout is compatible", async () => {
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
});'''
new_future_js = '''test("PLR rejects versions newer than Terraria's current release", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  model.version = 326;
  let handle = 0;
  try {
    handle = openJson(module, model);
    assert.equal(getField(module, handle, "/version"), 326);
    assert.equal(setField(module, handle, "/version", 327), 6,
      "release 327 must be rejected until Terraria defines that layout");
    assert.equal(getField(module, handle, "/version"), 326,
      "failed future-version edit must remain atomic");
  } finally {
    if (handle) module._terra_player_close(handle);
  }
  assert.equal(module._tx_heap_used(), baseline);
});'''
real_js = replace_once(real_js, old_future_js, new_future_js, "replace future-version JS contract")
real_js_path.write_text(real_js, encoding="utf-8")

# General PLR tests: remove the inferred v326 extension checks and add explicit
# 318..326 transition coverage around releases 322 and 324.
plr_js_path = Path("tests/test_plr.js")
plr_js = plr_js_path.read_text(encoding="utf-8")
plr_js = replace_count(plr_js, '''      assert.equal(original.formatExtensions?.v326PrefixFlag, true,
        "v326 extension byte regression");
''', '', 1, "remove Node inferred extension contract")
plr_js = replace_count(plr_js, '''      assert.equal(original.formatExtensions?.v326PrefixFlag, true);
''', '', 1, "remove Web inferred extension contract")
plr_js += r'''

test("PLR releases 318-326 follow Terraria's 322 favorite and 324 reserved-byte transitions", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const versions = [318, 319, 320, 321, 322, 323, 324, 325, 326];

  for (const version of versions) {
    const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
    model.version = version;
    model.armor[0].favorited = true;
    model.dyes[0].favorited = true;
    model.inventory[0].favorited = true;
    model.loadouts[0].armor[0].favorited = true;
    model.loadouts[0].dyes[0].favorited = true;
    model.ateArtisanBread = true;
    model.usedAegisCrystal = false;
    model.usedAegisFruit = true;
    model.usedArcaneCrystal = false;
    model.usedGalaxyPearl = true;
    model.usedGummyWorm = false;
    model.usedAmbrosia = true;

    let handle = 0;
    let reopened = 0;
    try {
      handle = openJson(module, model);
      reopened = openBuffer(module, encode(module, handle));
      assert.equal(getField(module, reopened, "/version"), version);
      const equipmentFavorite = version >= 322;
      assert.equal(getField(module, reopened, "/armor/0/favorited"), equipmentFavorite,
        `main armor favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/dyes/0/favorited"), equipmentFavorite,
        `main dye favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/loadouts/0/armor/0/favorited"), equipmentFavorite,
        `loadout armor favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/loadouts/0/dyes/0/favorited"), equipmentFavorite,
        `loadout dye favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/inventory/0/favorited"), true,
        `inventory favorite unexpectedly changed for release ${version}`);
      assert.equal(getField(module, reopened, "/ateArtisanBread"), true);
      assert.equal(getField(module, reopened, "/usedAegisCrystal"), false);
      assert.equal(getField(module, reopened, "/usedAegisFruit"), true);
      assert.equal(getField(module, reopened, "/usedArcaneCrystal"), false);
      assert.equal(getField(module, reopened, "/usedGalaxyPearl"), true);
      assert.equal(getField(module, reopened, "/usedGummyWorm"), false);
      assert.equal(getField(module, reopened, "/usedAmbrosia"), true);
    } finally {
      if (reopened) module._terra_player_close(reopened);
      if (handle) module._terra_player_close(handle);
    }
    assert.equal(module._tx_heap_used(), baseline,
      `PLR release ${version} transition test leaked tracked allocations`);
  }

  const tooOld = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  tooOld.version = 317;
  const tooNew = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
  tooNew.version = 327;
  for (const invalid of [tooOld, tooNew]) {
    const input = allocString(module, JSON.stringify(invalid));
    const output = module._tx_malloc(4);
    assert(output);
    try {
      assert.equal(module._terra_player_open_json(input, output), 6,
        `unsupported PLR release ${invalid.version} must be rejected`);
      assert.equal(module.HEAPU32[output >>> 2] >>> 0, 0);
    } finally {
      module._tx_free(output);
      module._tx_free(input);
    }
  }
  assert.equal(module._tx_heap_used(), baseline);
});
'''
plr_js_path.write_text(plr_js, encoding="utf-8")

# Native real fixture contract: drop the invented extension and assert that a
# future-version mutation is rejected atomically.
native_path = Path("tests/plr_real_fixture_contract.c")
native = native_path.read_text(encoding="utf-8")
native = replace_once(native, '    char *extension_flag = NULL;\n', '', "remove extension variable")
native = replace_once(native, '''    CHECK(get_field(handle, "/formatExtensions/v326PrefixFlag", &extension_flag) &&
        text_equal(extension_flag, "true"), "v326 prefix extension byte was not preserved");
''', '', "remove native extension assertion")
start = native.index('    /* Version is data, not a whitelist.')
end_marker = '    free(edited_name);\n'
end = native.index(end_marker, start)
replacement = '''    /* Terraria's current Player loader marks releases newer than 326 as
     * LaterVersion. Editing the semantic version must enforce the same bound
     * and must not partially commit the failed mutation. */
    CHECK(terra_plr_set(reopened, "/version", "327") ==
        TERRAX_WORLD_STATUS_VALIDATION_ERROR, "future PLR version was accepted");
    char *still_version = NULL;
    CHECK(get_field(reopened, "/version", &still_version), "read version after rejected edit");
    CHECK(text_equal(still_version, "326"), "rejected future version mutated the document");

    free(still_version);
'''
native = native[:start] + replacement + native[end:]
native = replace_once(native, '    free(extension_flag);\n', '', "remove extension free")
native = replace_once(native, '    CHECK(terra_plr_close(versioned_handle) == TERRAX_WORLD_STATUS_OK, "close versioned handle");\n', '', "remove obsolete future handle close")
if "version_777" in native or "versioned_handle" in native or "extension_flag" in native:
    raise SystemExit("obsolete future-version/extension native variables remain")
native_path.write_text(native, encoding="utf-8")

# WLD reader safety: lock the official current-version ceiling at 326.
reader_path = Path("tests/test_reader_safety.js")
reader = reader_path.read_text(encoding="utf-8")
reader += r'''

test("WLD current-version guard matches Terraria release 326", async () => {
  const M = await TerraWorldWasm();

  for (const [version, expectedCode] of [
    [326, "TERRAX_TRUNCATED_FORMAT"],
    [327, "TERRAX_UNSUPPORTED_VERSION"],
  ]) {
    const candidate = Buffer.alloc(4);
    candidate.writeUInt32LE(version, 0);
    const inputPtr = alloc(M, candidate);
    const handlePtr = M._tx_malloc(4);
    assert.notEqual(handlePtr, 0);
    try {
      const status = M._terra_world_open_from_buffer(inputPtr, candidate.length, handlePtr);
      assert.notEqual(status, 0);
      assert.equal(M.HEAPU32[handlePtr >>> 2] >>> 0, 0);
      assert.equal(readLastError(M).code, expectedCode,
        `unexpected WLD version guard for release ${version}`);
    } finally {
      M._tx_free(handlePtr);
      M._tx_free(inputPtr);
    }
  }
});
'''
reader_path.write_text(reader, encoding="utf-8")

# Fixture documentation should describe what the real v326 gate is proving.
readme_path = Path("tests/files/README.md")
readme = readme_path.read_text(encoding="utf-8")
readme += '''\n## Decompiled-source compatibility ledger\n\nThe current Terraria decompiled sources define release **326** as the latest supported Player/WLD version. TerraWasm intentionally supports semantic PLR versions **318 through 326**. Within that range, release 322 adds favorite bytes to main/loadout armor and dye slots, and release 324 adds one reserved boolean after `ateArtisanBread` which Terraria reads and discards. The reserved byte is not semantic JSON state. WLD V2 input is rejected above release 326 rather than guessed using a future layout.\n'''
readme_path.write_text(readme, encoding="utf-8")

print("source-aligned PLR/WLD transform applied")
