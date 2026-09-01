from pathlib import Path

SOURCE = Path("src/terra_plr.c")
NATIVE_TEST = Path("tests/plr_real_fixture_contract.c")
JS_TEST = Path("tests/test_plr.js")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


src = SOURCE.read_text(encoding="utf-8")

src = replace_once(
    src,
    "/* -------------------------------------------------------------------------\n * Semantic model readers\n * ------------------------------------------------------------------------- */\n",
    "static int plr_version_uses_v326_layout(int32_t version) {\n"
    "    /* v326 extends persisted equipment slots with a favorite byte and adds\n"
    "     * one body-prefix boolean. Higher versions are attempted using the most\n"
    "     * recent known layout; the strict EOF check rejects incompatible layouts. */\n"
    "    return version >= 326;\n"
    "}\n\n"
    "/* -------------------------------------------------------------------------\n * Semantic model readers\n * ------------------------------------------------------------------------- */\n",
    "insert version-layout helper",
)

src = replace_once(
    src,
    "static PlrJsonValue *plr_read_type_prefix_item(PlrReader *reader) {\n"
    "    int32_t item_type = plr_read_i32(reader);\n"
    "    uint8_t prefix = plr_read_u8(reader);\n"
    "    if (!reader->ok) return NULL;\n"
    "    return plr_make_item(item_type, 0, prefix, 0);\n"
    "}\n",
    "static PlrJsonValue *plr_read_type_prefix_item(\n"
    "    PlrReader *reader, int with_favorited) {\n"
    "    int32_t item_type = plr_read_i32(reader);\n"
    "    uint8_t prefix = plr_read_u8(reader);\n"
    "    int favorited = with_favorited ? (plr_read_u8(reader) != 0u) : 0;\n"
    "    if (!reader->ok) return NULL;\n"
    "    return plr_make_item(item_type, 0, prefix, favorited);\n"
    "}\n",
    "read type-prefix favorite",
)

src = replace_once(
    src,
    "        PlrJsonValue *item = type_prefix ?\n"
    "            plr_read_type_prefix_item(reader) : plr_read_full_item(reader, with_favorited);\n",
    "        PlrJsonValue *item = type_prefix ?\n"
    "            plr_read_type_prefix_item(reader, with_favorited) :\n"
    "            plr_read_full_item(reader, with_favorited);\n",
    "item-array read favorite",
)

src = replace_once(
    src,
    "static PlrJsonValue *plr_read_loadouts(PlrReader *reader) {\n",
    "static PlrJsonValue *plr_read_loadouts(\n    PlrReader *reader, int with_favorited) {\n",
    "loadout reader signature",
)
src = replace_once(
    src,
    "        PlrJsonValue *armor = plr_read_item_array(reader, PLR_ARMOR_SLOTS, 0, 0);\n"
    "        PlrJsonValue *dyes = plr_read_item_array(reader, PLR_DYE_SLOTS, 0, 0);\n",
    "        PlrJsonValue *armor = plr_read_item_array(\n"
    "            reader, PLR_ARMOR_SLOTS, with_favorited, 0);\n"
    "        PlrJsonValue *dyes = plr_read_item_array(\n"
    "            reader, PLR_DYE_SLOTS, with_favorited, 0);\n",
    "loadout reader favorite arrays",
)

src = replace_once(
    src,
    "    int32_t version = plr_read_i32(&reader);\n    if (!reader.ok) return NULL;\n\n",
    "    int32_t version = plr_read_i32(&reader);\n"
    "    if (!reader.ok) return NULL;\n"
    "    const int v326_layout = plr_version_uses_v326_layout(version);\n\n",
    "parse version capability",
)

src = replace_once(
    src,
    "        !plr_put_reader_bool(root, \"usedAmbrosia\", &reader) ||\n"
    "        !plr_put_reader_bool(root, \"downedDd2EventAnyDifficulty\", &reader) ||\n"
    "        !plr_put_reader_i32(root, \"taxMoney\", &reader) ||\n"
    "        !plr_put_reader_i32(root, \"numberOfDeathsPve\", &reader) ||\n"
    "        !plr_put_reader_i32(root, \"numberOfDeathsPvp\", &reader) || !reader.ok) {\n"
    "        plr_json_free(root);\n"
    "        return NULL;\n"
    "    }\n",
    "        !plr_put_reader_bool(root, \"usedAmbrosia\", &reader) ||\n"
    "        !plr_put_reader_bool(root, \"downedDd2EventAnyDifficulty\", &reader) || !reader.ok) {\n"
    "        plr_json_free(root);\n"
    "        return NULL;\n"
    "    }\n"
    "    if (v326_layout) {\n"
    "        int extension_flag = plr_read_u8(&reader) != 0u;\n"
    "        PlrJsonValue *extensions = plr_json_object();\n"
    "        if (!reader.ok || !extensions ||\n"
    "            !plr_json_object_put_bool(extensions, \"v326PrefixFlag\", extension_flag) ||\n"
    "            !plr_root_put(root, \"formatExtensions\", extensions)) {\n"
    "            plr_json_free(extensions);\n"
    "            plr_json_free(root);\n"
    "            return NULL;\n"
    "        }\n"
    "        extensions = NULL;\n"
    "    }\n"
    "    if (!plr_put_reader_i32(root, \"taxMoney\", &reader) ||\n"
    "        !plr_put_reader_i32(root, \"numberOfDeathsPve\", &reader) ||\n"
    "        !plr_put_reader_i32(root, \"numberOfDeathsPvp\", &reader) || !reader.ok) {\n"
    "        plr_json_free(root);\n"
    "        return NULL;\n"
    "    }\n",
    "parse v326 prefix extension",
)

src = replace_once(
    src,
    "    PlrJsonValue *armor = plr_read_item_array(&reader, PLR_ARMOR_SLOTS, 0, 1);\n"
    "    PlrJsonValue *dyes = plr_read_item_array(&reader, PLR_DYE_SLOTS, 0, 1);\n",
    "    PlrJsonValue *armor = plr_read_item_array(\n"
    "        &reader, PLR_ARMOR_SLOTS, v326_layout, 1);\n"
    "    PlrJsonValue *dyes = plr_read_item_array(\n"
    "        &reader, PLR_DYE_SLOTS, v326_layout, 1);\n",
    "main equipment v326 favorites",
)

src = src.replace("plr_read_type_prefix_item(&reader);", "plr_read_type_prefix_item(&reader, 0);")
if src.count("plr_read_type_prefix_item(&reader, 0);") < 2:
    raise RuntimeError("misc type-prefix reader replacements missing")

src = replace_once(
    src,
    "    PlrJsonValue *loadouts = plr_read_loadouts(&reader);\n",
    "    PlrJsonValue *loadouts = plr_read_loadouts(&reader, v326_layout);\n",
    "parse loadouts capability",
)

src = replace_once(
    src,
    "    if (!plr_required_i32(root, \"version\"))\n"
    "        return plr_model_error(\"PLR version is missing or invalid\");\n\n",
    "    int32_t model_version = 0;\n"
    "    if (!plr_value_i32(plr_json_object_get(root, \"version\"), &model_version))\n"
    "        return plr_model_error(\"PLR version is missing or invalid\");\n"
    "    const PlrJsonValue *format_extensions =\n"
    "        plr_json_object_get(root, \"formatExtensions\");\n"
    "    if (format_extensions) {\n"
    "        if (!plr_version_uses_v326_layout(model_version) ||\n"
    "            format_extensions->type != PLR_JSON_OBJECT ||\n"
    "            !plr_required_bool(format_extensions, \"v326PrefixFlag\"))\n"
    "            return plr_model_error(\"PLR formatExtensions is invalid for this version\");\n"
    "    }\n\n",
    "validate format extensions",
)

src = replace_once(
    src,
    "static void plr_writer_type_prefix_item(\n"
    "    PlrWriter *writer, const PlrJsonValue *item) {\n"
    "    int32_t item_type = 0;\n"
    "    uint8_t prefix = 0;\n"
    "    if (!plr_field_i32(item, \"itemType\", &item_type) ||\n"
    "        !plr_field_u8(item, \"prefix\", &prefix)) {\n"
    "        writer->ok = 0;\n"
    "        return;\n"
    "    }\n"
    "    plr_writer_i32(writer, item_type);\n"
    "    plr_writer_u8(writer, prefix);\n"
    "}\n",
    "static void plr_writer_type_prefix_item(\n"
    "    PlrWriter *writer, const PlrJsonValue *item, int with_favorited) {\n"
    "    int32_t item_type = 0;\n"
    "    uint8_t prefix = 0;\n"
    "    int favorited = 0;\n"
    "    if (!plr_field_i32(item, \"itemType\", &item_type) ||\n"
    "        !plr_field_u8(item, \"prefix\", &prefix) ||\n"
    "        (with_favorited && !plr_field_bool(item, \"favorited\", &favorited))) {\n"
    "        writer->ok = 0;\n"
    "        return;\n"
    "    }\n"
    "    plr_writer_i32(writer, item_type);\n"
    "    plr_writer_u8(writer, prefix);\n"
    "    if (with_favorited) plr_writer_u8(writer, favorited ? 1u : 0u);\n"
    "}\n",
    "write type-prefix favorite",
)

src = replace_once(
    src,
    "        if (type_prefix) plr_writer_type_prefix_item(writer, array->as.array.items[i]);\n"
    "        else plr_writer_item(writer, array->as.array.items[i], with_favorited);\n",
    "        if (type_prefix)\n"
    "            plr_writer_type_prefix_item(\n"
    "                writer, array->as.array.items[i], with_favorited);\n"
    "        else plr_writer_item(writer, array->as.array.items[i], with_favorited);\n",
    "item-array writer favorite",
)

src = replace_once(
    src,
    "static void plr_writer_loadouts(PlrWriter *writer, const PlrJsonValue *root) {\n",
    "static void plr_writer_loadouts(\n    PlrWriter *writer, const PlrJsonValue *root, int with_favorited) {\n",
    "loadout writer signature",
)
src = replace_once(
    src,
    "        plr_writer_item_array(writer, loadout, \"armor\", PLR_ARMOR_SLOTS, 0, 0);\n"
    "        plr_writer_item_array(writer, loadout, \"dyes\", PLR_DYE_SLOTS, 0, 0);\n",
    "        plr_writer_item_array(\n"
    "            writer, loadout, \"armor\", PLR_ARMOR_SLOTS, with_favorited, 0);\n"
    "        plr_writer_item_array(\n"
    "            writer, loadout, \"dyes\", PLR_DYE_SLOTS, with_favorited, 0);\n",
    "loadout writer favorites",
)

src = replace_once(
    src,
    "    if (!plr_field_i32(root, \"version\", &version)) writer.ok = 0;\n"
    "    plr_writer_i32(&writer, version);\n",
    "    if (!plr_field_i32(root, \"version\", &version)) writer.ok = 0;\n"
    "    const int v326_layout = plr_version_uses_v326_layout(version);\n"
    "    plr_writer_i32(&writer, version);\n",
    "writer version capability",
)

src = replace_once(
    src,
    "    for (uint32_t index = 0u; index < 11u; index++) {\n"
    "        if (!plr_field_bool(root, body_bool_fields[index], &boolean)) writer.ok = 0;\n"
    "        plr_writer_u8(&writer, boolean ? 1u : 0u);\n"
    "    }\n"
    "    const char *death_fields[] = {\"taxMoney\", \"numberOfDeathsPve\", \"numberOfDeathsPvp\"};\n",
    "    for (uint32_t index = 0u; index < 11u; index++) {\n"
    "        if (!plr_field_bool(root, body_bool_fields[index], &boolean)) writer.ok = 0;\n"
    "        plr_writer_u8(&writer, boolean ? 1u : 0u);\n"
    "    }\n"
    "    if (v326_layout) {\n"
    "        int extension_flag = 0;\n"
    "        const PlrJsonValue *extensions = plr_field(root, \"formatExtensions\");\n"
    "        if (extensions) {\n"
    "            if (extensions->type != PLR_JSON_OBJECT ||\n"
    "                !plr_field_bool(extensions, \"v326PrefixFlag\", &extension_flag))\n"
    "                writer.ok = 0;\n"
    "        }\n"
    "        plr_writer_u8(&writer, extension_flag ? 1u : 0u);\n"
    "    }\n"
    "    const char *death_fields[] = {\"taxMoney\", \"numberOfDeathsPve\", \"numberOfDeathsPvp\"};\n",
    "write v326 prefix extension",
)

src = replace_once(
    src,
    "    plr_writer_item_array(&writer, root, \"armor\", PLR_ARMOR_SLOTS, 0, 1);\n"
    "    plr_writer_item_array(&writer, root, \"dyes\", PLR_DYE_SLOTS, 0, 1);\n",
    "    plr_writer_item_array(\n"
    "        &writer, root, \"armor\", PLR_ARMOR_SLOTS, v326_layout, 1);\n"
    "    plr_writer_item_array(\n"
    "        &writer, root, \"dyes\", PLR_DYE_SLOTS, v326_layout, 1);\n",
    "write main equipment favorites",
)

src = src.replace(
    "plr_writer_type_prefix_item(&writer, misc_equips->as.array.items[index]);",
    "plr_writer_type_prefix_item(&writer, misc_equips->as.array.items[index], 0);",
)
src = src.replace(
    "plr_writer_type_prefix_item(&writer, misc_dyes->as.array.items[index]);",
    "plr_writer_type_prefix_item(&writer, misc_dyes->as.array.items[index], 0);",
)

src = replace_once(
    src,
    "    plr_writer_loadouts(&writer, root);\n",
    "    plr_writer_loadouts(&writer, root, v326_layout);\n",
    "write loadouts capability",
)

SOURCE.write_text(src, encoding="utf-8")

native = NATIVE_TEST.read_text(encoding="utf-8")
native = replace_once(
    native,
    "    char *name = NULL;\n"
    "    char *version = NULL;\n"
    "    CHECK(get_field(handle, \"/name\", &name), \"read real player name\");\n"
    "    CHECK(name[0] == '\"' && name[1] != '\"', \"real player name is empty\");\n"
    "    CHECK(get_field(handle, \"/version\", &version), \"read real player version\");\n",
    "    char *name = NULL;\n"
    "    char *version = NULL;\n"
    "    char *tax_money = NULL;\n"
    "    char *deaths_pve = NULL;\n"
    "    char *voice_variant = NULL;\n"
    "    char *extension_flag = NULL;\n"
    "    CHECK(get_field(handle, \"/name\", &name), \"read real player name\");\n"
    "    CHECK(name[0] == '\"' && name[1] != '\"', \"real player name is empty\");\n"
    "    CHECK(get_field(handle, \"/version\", &version), \"read real player version\");\n"
    "    CHECK(text_equal(version, \"326\"), \"real fixture is not the expected v326 player\");\n"
    "    CHECK(get_field(handle, \"/taxMoney\", &tax_money) && text_equal(tax_money, \"113750\"),\n"
    "        \"v326 body prefix is misaligned before taxMoney\");\n"
    "    CHECK(get_field(handle, \"/numberOfDeathsPve\", &deaths_pve) && text_equal(deaths_pve, \"11\"),\n"
    "        \"v326 death counters are misaligned\");\n"
    "    CHECK(get_field(handle, \"/voiceVariant\", &voice_variant) && text_equal(voice_variant, \"2\"),\n"
    "        \"v326 loadout favorite bytes are misaligned before voiceVariant\");\n"
    "    CHECK(get_field(handle, \"/formatExtensions/v326PrefixFlag\", &extension_flag) &&\n"
    "        text_equal(extension_flag, \"true\"), \"v326 prefix extension byte was not preserved\");\n",
    "native v326 semantic assertions",
)
native = replace_once(
    native,
    "    free(version_777);\n"
    "    free(versioned);\n"
    "    free(edited_name);\n"
    "    free(edited);\n"
    "    free(version);\n"
    "    free(name);\n",
    "    free(version_777);\n"
    "    free(versioned);\n"
    "    free(edited_name);\n"
    "    free(edited);\n"
    "    free(extension_flag);\n"
    "    free(voice_variant);\n"
    "    free(deaths_pve);\n"
    "    free(tax_money);\n"
    "    free(version);\n"
    "    free(name);\n",
    "native assertion cleanup",
)
NATIVE_TEST.write_text(native, encoding="utf-8")

js = JS_TEST.read_text(encoding="utf-8")
js = replace_once(
    js,
    "    const original = twoCallJson(module, module._terra_player_get_json, [handle]);\n"
    "    assert.equal(typeof original.name, \"string\");\n"
    "    assert.notEqual(original.name.length, 0);\n",
    "    const original = twoCallJson(module, module._terra_player_get_json, [handle]);\n"
    "    assert.equal(typeof original.name, \"string\");\n"
    "    assert.notEqual(original.name.length, 0);\n"
    "    if (EXTERNAL_FIXTURE) {\n"
    "      assert.equal(original.version, 326, \"real fixture version regression\");\n"
    "      assert.equal(original.taxMoney, 113750, \"v326 body-prefix alignment regression\");\n"
    "      assert.equal(original.numberOfDeathsPve, 11, \"v326 death alignment regression\");\n"
    "      assert.equal(original.voiceVariant, 2, \"v326 loadout alignment regression\");\n"
    "      assert.equal(original.formatExtensions?.v326PrefixFlag, true,\n"
    "        \"v326 extension byte regression\");\n"
    "    }\n",
    "node v326 semantic assertions",
)
js = replace_once(
    js,
    "    handle = openBuffer(module, source);\n"
    "    const originalName = getField(module, handle, \"/name\");\n",
    "    handle = openBuffer(module, source);\n"
    "    if (EXTERNAL_FIXTURE) {\n"
    "      const original = twoCallJson(module, module._terra_player_get_json, [handle]);\n"
    "      assert.equal(original.version, 326);\n"
    "      assert.equal(original.taxMoney, 113750);\n"
    "      assert.equal(original.voiceVariant, 2);\n"
    "      assert.equal(original.formatExtensions?.v326PrefixFlag, true);\n"
    "    }\n"
    "    const originalName = getField(module, handle, \"/name\");\n",
    "web v326 semantic assertions",
)
JS_TEST.write_text(js, encoding="utf-8")
