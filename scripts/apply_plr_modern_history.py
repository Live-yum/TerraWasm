from pathlib import Path


def one(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 match, got {n}")
    return text.replace(old, new, 1)

p = Path('src/terra_plr.c')
s = p.read_text()

s = one(s, '#define PLR_MIN_SUPPORTED_VERSION 318\n', '#define PLR_MIN_SUPPORTED_VERSION 280\n', 'PLR min')
s = one(s, '''static int plr_version_has_reserved_324(int32_t version) {
    /* Player.SavePlayer writes a reserved false boolean immediately after
     * ateArtisanBread; Player.LoadPlayer_Version2 consumes it from release 324. */
    return version >= 324;
}
''', '''static int plr_version_has_voice_pitch(int32_t version) {
    return version >= 281;
}

static int plr_version_has_team(int32_t version) {
    return version >= 283;
}

static int plr_version_has_pending_refunds(int32_t version) {
    return version >= 300;
}

static int plr_version_has_dialogues(int32_t version) {
    return version >= 310;
}

static int plr_version_has_reserved_324(int32_t version) {
    /* Player.SavePlayer writes a reserved false boolean immediately after
     * ateArtisanBread; Player.LoadPlayer_Version2 consumes it from release 324. */
    return version >= 324;
}
''', 'history helpers')

state_old = '''    const int equipment_favorites = plr_version_has_equipment_favorites(version);
    const int reserved_324 = plr_version_has_reserved_324(version);
'''
state_new = '''    const int voice_pitch = plr_version_has_voice_pitch(version);
    const int team = plr_version_has_team(version);
    const int pending_refunds = plr_version_has_pending_refunds(version);
    const int dialogues_seen = plr_version_has_dialogues(version);
    const int equipment_favorites = plr_version_has_equipment_favorites(version);
    const int reserved_324 = plr_version_has_reserved_324(version);
'''
if s.count(state_old) != 2:
    raise SystemExit(f'version state: expected 2 matches, got {s.count(state_old)}')
s = s.replace(state_old, state_new)

s = one(s, '''    if (!plr_json_object_put_string_owned(root, "name", owned_name) ||
        !plr_json_object_put_u64(root, "difficulty", plr_read_u8(&reader)) ||
        !plr_put_reader_i64(root, "playTimeTicks", &reader) ||
        !plr_put_reader_i32(root, "hair", &reader) ||
        !plr_put_reader_u8(root, "hairDye", &reader) ||
        !plr_put_reader_u8(root, "team", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }
''', '''    if (!plr_json_object_put_string_owned(root, "name", owned_name) ||
        !plr_json_object_put_u64(root, "difficulty", plr_read_u8(&reader)) ||
        !plr_put_reader_i64(root, "playTimeTicks", &reader) ||
        !plr_put_reader_i32(root, "hair", &reader) ||
        !plr_put_reader_u8(root, "hairDye", &reader) || !reader.ok) {
        plr_json_free(root);
        return NULL;
    }
    uint8_t team_value = team ? plr_read_u8(&reader) : 0u;
    if (!reader.ok || !plr_json_object_put_u64(root, "team", team_value)) {
        plr_json_free(root);
        return NULL;
    }
''', 'team reader gate')

s = one(s, '''    if (!plr_root_put(root, "loadouts", loadouts) ||
        !plr_put_reader_u8(root, "voiceVariant", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }
    float voice_pitch = plr_read_f32(&reader);
    if (!reader.ok || !plr_json_object_put_float(root, "voicePitchOffset", voice_pitch)) {
        plr_json_free(root); return NULL;
    }
    uint32_t pending_count = 0u;
    if (!plr_read_count(&reader, PLR_MAX_PENDING_REFUNDS, 9u, &pending_count)) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *pending = plr_read_item_array(&reader, pending_count, 0, 0);
    PlrJsonValue *dialogues = plr_read_dialogues(&reader);
    if (!pending || !dialogues) {
        plr_json_free(pending); plr_json_free(dialogues); plr_json_free(root); return NULL;
    }
''', '''    if (!plr_root_put(root, "loadouts", loadouts) ||
        !plr_put_reader_u8(root, "voiceVariant", &reader) || !reader.ok) {
        plr_json_free(root); return NULL;
    }
    float voice_pitch_value = voice_pitch ? plr_read_f32(&reader) : 0.0f;
    if (!reader.ok || !plr_json_object_put_float(root, "voicePitchOffset", voice_pitch_value)) {
        plr_json_free(root); return NULL;
    }
    uint32_t pending_count = 0u;
    if (pending_refunds &&
        !plr_read_count(&reader, PLR_MAX_PENDING_REFUNDS, 9u, &pending_count)) {
        plr_json_free(root); return NULL;
    }
    PlrJsonValue *pending = plr_read_item_array(&reader, pending_count, 0, 0);
    PlrJsonValue *dialogues = dialogues_seen ? plr_read_dialogues(&reader) : plr_json_array();
    if (!pending || !dialogues) {
        plr_json_free(pending); plr_json_free(dialogues); plr_json_free(root); return NULL;
    }
''', 'tail reader gates')

s = one(s, '''        return plr_model_error("PLR version must be within the supported Terraria 318-326 range");
''', '''        return plr_model_error("PLR version must be within TerraWasm's modern Terraria 280-326 range");
''', 'validation message')

s = one(s, '''    if (!plr_field_u8(root, "hairDye", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
    if (!plr_field_u8(root, "team", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
''', '''    if (!plr_field_u8(root, "hairDye", &u8)) writer.ok = 0;
    plr_writer_u8(&writer, u8);
    if (!plr_field_u8(root, "team", &u8)) writer.ok = 0;
    if (team) plr_writer_u8(&writer, u8);
''', 'team writer gate')

s = one(s, '''    float voice_pitch = 0.0f;
    if (!plr_field_float(root, "voicePitchOffset", &voice_pitch)) writer.ok = 0;
    plr_writer_f32(&writer, voice_pitch);
    const PlrJsonValue *pending = plr_field_array(root, "pendingRefunds");
    if (!pending || pending->as.array.count > PLR_MAX_PENDING_REFUNDS ||
        pending->as.array.count > INT32_MAX) writer.ok = 0;
    else {
        plr_writer_i32(&writer, (int32_t)pending->as.array.count);
        for (uint32_t index = 0u; index < pending->as.array.count; index++)
            plr_writer_item(&writer, pending->as.array.items[index], 0);
    }
    const PlrJsonValue *dialogues = plr_field_array(root, "oneTimeDialoguesSeen");
    if (!dialogues || dialogues->as.array.count > PLR_MAX_DIALOGUES ||
        dialogues->as.array.count > INT32_MAX) writer.ok = 0;
    else {
        plr_writer_i32(&writer, (int32_t)dialogues->as.array.count);
        for (uint32_t index = 0u; index < dialogues->as.array.count; index++) {
            const char *dialogue = NULL;
            if (!plr_value_string(dialogues->as.array.items[index], &dialogue)) writer.ok = 0;
            plr_writer_string(&writer, dialogue ? dialogue : "");
        }
    }
''', '''    float voice_pitch_value = 0.0f;
    if (!plr_field_float(root, "voicePitchOffset", &voice_pitch_value)) writer.ok = 0;
    if (voice_pitch) plr_writer_f32(&writer, voice_pitch_value);
    const PlrJsonValue *pending = plr_field_array(root, "pendingRefunds");
    if (!pending || pending->as.array.count > PLR_MAX_PENDING_REFUNDS ||
        pending->as.array.count > INT32_MAX) writer.ok = 0;
    else if (pending_refunds) {
        plr_writer_i32(&writer, (int32_t)pending->as.array.count);
        for (uint32_t index = 0u; index < pending->as.array.count; index++)
            plr_writer_item(&writer, pending->as.array.items[index], 0);
    }
    const PlrJsonValue *dialogues = plr_field_array(root, "oneTimeDialoguesSeen");
    if (!dialogues || dialogues->as.array.count > PLR_MAX_DIALOGUES ||
        dialogues->as.array.count > INT32_MAX) writer.ok = 0;
    else if (dialogues_seen) {
        plr_writer_i32(&writer, (int32_t)dialogues->as.array.count);
        for (uint32_t index = 0u; index < dialogues->as.array.count; index++) {
            const char *dialogue = NULL;
            if (!plr_value_string(dialogues->as.array.items[index], &dialogue)) writer.ok = 0;
            plr_writer_string(&writer, dialogue ? dialogue : "");
        }
    }
''', 'tail writer gates')

p.write_text(s)

# Expand the JS transition matrix and verify every source-defined boundary.
tp = Path('tests/test_plr.js')
t = tp.read_text()
start = t.index('test("PLR releases 318-326 follow Terraria')
replacement = r'''test("PLR releases 280-326 follow Terraria's modern history gates", async () => {
  const module = await TerraWorldWasm();
  const baseline = module._tx_heap_used();
  const versions = [280, 281, 282, 283, 299, 300, 309, 310, 321, 322, 323, 324, 325, 326];

  for (const version of versions) {
    const model = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
    model.version = version;
    model.team = 7;
    model.voiceVariant = 3;
    model.voicePitchOffset = 0.25;
    model.pendingRefunds = [{ itemType: 1, stack: 2, prefix: 0, favorited: false }];
    model.oneTimeDialoguesSeen = ["source-gate"];
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
      assert.equal(getField(module, reopened, "/voiceVariant"), 3);
      assert.equal(getField(module, reopened, "/voicePitchOffset"), version >= 281 ? 0.25 : 0,
        `voice pitch gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/team"), version >= 283 ? 7 : 0,
        `team gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/pendingRefunds").length, version >= 300 ? 1 : 0,
        `pending refund gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/oneTimeDialoguesSeen").length, version >= 310 ? 1 : 0,
        `dialogue gate mismatch for release ${version}`);
      const equipmentFavorite = version >= 322;
      assert.equal(getField(module, reopened, "/armor/0/favorited"), equipmentFavorite,
        `main armor favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/dyes/0/favorited"), equipmentFavorite,
        `main dye favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/loadouts/0/armor/0/favorited"), equipmentFavorite,
        `loadout armor favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/loadouts/0/dyes/0/favorited"), equipmentFavorite,
        `loadout dye favorite gate mismatch for release ${version}`);
      assert.equal(getField(module, reopened, "/inventory/0/favorited"), true);
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
      `PLR release ${version} history test leaked tracked allocations`);
  }

  for (const version of [279, 327]) {
    const invalid = JSON.parse(fs.readFileSync(MODEL_FIXTURE, "utf8"));
    invalid.version = version;
    const input = allocString(module, JSON.stringify(invalid));
    const output = module._tx_malloc(4);
    assert(output);
    try {
      assert.equal(module._terra_player_open_json(input, output), 6,
        `unsupported PLR release ${version} must be rejected`);
      assert.equal(module.HEAPU32[output >>> 2] >>> 0, 0);
    } finally {
      module._tx_free(output);
      module._tx_free(input);
    }
  }
  assert.equal(module._tx_heap_used(), baseline);
});'''
t = t[:start] + replacement + '\n'
tp.write_text(t)

r = Path('tests/files/README.md')
rs = r.read_text()
rs = rs.replace('semantic PLR versions **318 through 326**', 'modern-profile PLR versions **280 through 326**')
rs = rs.replace('Within that range, release 322 adds favorite bytes to main/loadout armor and dye slots, and release 324 adds one reserved boolean after `ateArtisanBread` which Terraria reads and discards.', 'Within that range, release 281 adds voice pitch, 283 adds team, 300 adds pending refunds, 310 adds one-time dialogues, 322 adds favorite bytes to main/loadout armor and dye slots, and 324 adds one reserved boolean after `ateArtisanBread` which Terraria reads and discards.')
r.write_text(rs)
print('PLR 280-326 modern-history compatibility applied')
